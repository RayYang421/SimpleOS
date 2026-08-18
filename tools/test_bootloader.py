#!/usr/bin/env python3
"""End-to-end test: boot the bootloader in QEMU, send the kernel over the
emulated UART, then drive the shell and check what comes back.

Usage:
    tools/test_bootloader.py [kernel.img]

Exits non-zero if any check fails, so it works as a regression test.
"""

import os
import re
import select
import struct
import subprocess
import sys
import time

MAGIC = b"OSCK"
ACK = 0x06
NAK = 0x15

# Every wait below is wall-clock, but what it is really waiting for is the
# guest making progress -- and under icount the guest runs as fast as the host
# can emulate it. A slower machine (a CI runner, say) needs longer for the same
# amount of guest time, so the waits scale rather than being retuned.
WAIT_SCALE = float(os.environ.get("TEST_WAIT_SCALE", "1"))

BOOTLOADER = "bootloader/build/bootloader.img"
KERNEL = "build/kernel8.img"
INITRAMFS = "initramfs.cpio"
DTB = "bcm2710-rpi-3-b-plus.dtb"
SD_IMAGE = "sd.img"


class Board:
    """QEMU running the bootloader, talked to over its serial port."""

    def __init__(self):
        self.proc = subprocess.Popen(
            ["qemu-system-aarch64", "-M", "raspi3b",
             # raspi3b always has four cores and QEMU spins the three it is not
             # booting, which starves its own timer delivery and makes the
             # guest see core-timer interrupts up to a second late. Running the
             # cores on one host thread and driving the clock from instructions
             # retired makes the timing checks below deterministic.
             "-accel", "tcg,thread=single",
             "-icount", "shift=auto,sleep=on",
             "-kernel", BOOTLOADER,
             "-initrd", INITRAMFS,
             "-dtb", DTB,
             "-drive", f"if=sd,file={SD_IMAGE},format=raw",
             "-display", "none", "-serial", "null", "-serial", "stdio"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL)
        self.buf = bytearray()      # everything received, for the transcript
        self.cursor = 0             # how much of buf has been searched

    def _pump(self, timeout):
        ready, _, _ = select.select([self.proc.stdout], [], [], timeout)
        if not ready:
            return False
        chunk = os.read(self.proc.stdout.fileno(), 65536)
        if not chunk:
            return False
        self.buf += chunk
        return True

    def wait_for_status(self, timeout, what):
        """Scan the stream for the next ACK/NAK control byte."""
        timeout *= WAIT_SCALE
        deadline = time.time() + timeout
        while True:
            while self.cursor < len(self.buf):
                byte = self.buf[self.cursor]
                self.cursor += 1
                if byte == ACK:
                    return
                if byte == NAK:
                    time.sleep(0.3)
                    self._pump(0.5)
                    fail(f"bootloader sent NAK for the {what}", self.transcript())
            remaining = deadline - time.time()
            if remaining <= 0 or not self._pump(remaining):
                if time.time() >= deadline:
                    fail(f"no response to the {what} within {timeout:.0f}s",
                         self.transcript())

    def wait_for_text(self, needle, timeout):
        """Wait until needle appears anywhere in the output received so far."""
        timeout *= WAIT_SCALE
        deadline = time.time() + timeout
        target = needle.encode()
        while True:
            if target in self.buf:
                return True
            remaining = deadline - time.time()
            if remaining <= 0 or not self._pump(remaining):
                if time.time() >= deadline:
                    return False

    def send(self, data):
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def send_line(self, text):
        # Terminals send CR for Enter; the kernel's uart_getc maps it to '\n'.
        self.send(text.encode() + b"\r")

    def drain(self, seconds, scale=True):
        deadline = time.time() + seconds * (WAIT_SCALE if scale else 1)
        while time.time() < deadline:
            self._pump(max(0.0, deadline - time.time()))

    def transcript(self):
        return self.buf.decode(errors="replace")

    def kill(self):
        self.proc.kill()
        self.proc.wait(timeout=5)


def run_on_corrupt_card(kernel_path, kind, commands, seconds):
    """Boots the kernel directly against a deliberately broken card.

    Directly rather than through the bootloader, because this is about what
    the file system does with the card and the transfer has already been
    tested; it keeps the extra boot down to a few seconds."""
    image = "sd-corrupt.img"
    subprocess.run([sys.executable, "tools/make_sdcard.py", "corrupt", image, kind],
                   check=True, stdout=subprocess.DEVNULL)

    proc = subprocess.Popen(
        ["qemu-system-aarch64", "-M", "raspi3b",
         "-accel", "tcg,thread=single", "-icount", "shift=auto,sleep=on",
         "-kernel", kernel_path, "-initrd", INITRAMFS, "-dtb", DTB,
         "-drive", f"if=sd,file={image},format=raw",
         "-display", "none", "-serial", "null", "-serial", "stdio"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL)

    before = os.path.getsize(image), open(image, "rb").read()
    buf = bytearray()
    deadline = time.time() + seconds * WAIT_SCALE

    try:
        for command in commands:
            proc.stdin.write(command.encode() + b"\r")
            proc.stdin.flush()

        while time.time() < deadline:
            ready, _, _ = select.select([proc.stdout], [], [],
                                        max(0.0, deadline - time.time()))
            if not ready:
                break
            chunk = os.read(proc.stdout.fileno(), 65536)
            if not chunk:
                break
            buf += chunk
    finally:
        proc.kill()
        proc.wait(timeout=5)

    after = os.path.getsize(image), open(image, "rb").read()
    return buf.decode(errors="replace"), before == after


def fail(message, transcript=None):
    print(f"\n[FAIL] {message}")
    if transcript is not None:
        print("--- transcript " + "-" * 50)
        print(transcript)
        print("-" * 65)
    sys.exit(1)


def check(condition, description, transcript):
    status = "ok  " if condition else "FAIL"
    print(f"  [{status}] {description}")
    if not condition:
        fail(f"check failed: {description}", transcript)


def main():
    kernel_path = sys.argv[1] if len(sys.argv) > 1 else KERNEL

    for path in (BOOTLOADER, kernel_path, INITRAMFS, DTB):
        if not os.path.exists(path):
            raise SystemExit(f"missing {path} -- run `make all bootloader` first")

    # A fresh card every run: the kernel writes to it, so a second run would
    # otherwise start with the first run's files already there and the check
    # that an unsynced file never arrived would pass for the wrong reason.
    print("Building a fresh SD card image...")
    subprocess.run([sys.executable, "tools/make_sdcard.py", "build", SD_IMAGE],
                   check=True, stdout=subprocess.DEVNULL)

    with open(kernel_path, "rb") as f:
        image = f.read()
    checksum = sum(image) & 0xFFFFFFFF

    board = Board()
    try:
        print("Booting bootloader in QEMU...")
        if not board.wait_for_text("waiting for kernel", 15):
            fail("bootloader never reached its prompt", board.transcript())

        print(f"Sending kernel ({len(image)} bytes, checksum 0x{checksum:08x})...")
        board.send(MAGIC + struct.pack("<II", len(image), checksum))
        board.wait_for_status(10, "header")

        start = time.time()
        board.send(image)
        board.wait_for_status(30, "image")
        print(f"Kernel accepted in {time.time() - start:.2f}s.")

        if not board.wait_for_text("my-os kernel", 15):
            fail("kernel did not start", board.transcript())
        if not board.wait_for_text("Welcome to my-os shell", 10):
            fail("shell did not start", board.transcript())

        # (command, seconds to wait afterwards, whether that wait scales).
        # The lab 3 commands need real time to pass, because that is what they
        # are demonstrating.
        #
        # A wait that is there to let something finish scales with the machine.
        # One that aims at a moment *inside* a running program must not: a
        # slower machine stretches the program too, so scaling the delay would
        # walk it off the end instead of keeping it in the middle.
        script = [
            ("help", 0.6), ("hello", 0.6), ("ls", 0.6),
            ("cat hello.txt", 0.6), ("cat dir/nested.txt", 0.6),
            ("cat nope.txt", 0.6),
            ("malloc 64", 0.6), ("malloc 32", 0.6),
            ("dtb", 0.6), ("info", 0.6),
            ("exc", 1.5),
            ("setTimeout third 6", 0.4),
            ("setTimeout first 2", 0.4),
            ("setTimeout second 4", 8.0),
            ("timer", 5.0), ("timer", 0.6),
            ("tasktest", 4.0),
            ("irqinfo", 0.8),
            ("meminfo", 1.0),
            ("memtest", 2.5),
            ("palloc 2", 0.8),
            ("ps", 0.6),
            ("kthread 3", 4.0),
            ("ps", 0.6),
            # Bracketing the user programs, to show every frame they used came
            # back when they exited.
            ("meminfo", 1.5),
            ("exec hello.img", 0.05, False),
            ("vm", 0.10, False),
            ("vm", 0.20, False),
            ("vm", 8.0),
            ("exec sig.img", 0.05, False),
            ("vm", 4.0),
            ("exec vm.img", 0.30, False),
            ("vm", 12.0),
            ("mbox", 0.8),
            ("meminfo", 1.5),
            # After the frame-count check: the file system keeps what it is
            # given, so files created here are not a leak but would look like
            # one.
            ("fs ls /", 0.6),
            ("fs ls /initramfs", 0.6),
            ("fs ls /initramfs/dir", 0.6),
            ("fs cat /initramfs/dir/nested.txt", 0.6),
            ("fs mkdir /tmp", 0.6),
            ("fs write /tmp/note.txt vfs-round-trip", 0.6),
            ("fs cat /tmp/note.txt", 0.6),
            ("fs write /initramfs/x.txt nope", 0.6),
            ("fs cd /initramfs", 0.6),
            ("fs ls ..", 0.6),
            ("fs cd /", 0.6),
            ("exec fs.img", 10.0),
            ("sd", 0.8),
            ("fs ls /boot", 0.8),
            ("fs cat /boot/FAT_R.TXT", 0.8),
            ("exec fat.img", 10.0),
        ]

        print("\nDriving the shell:")
        for entry in script:
            command, wait = entry[0], entry[1]
            board.send_line(command)
            board.drain(wait, scale=entry[2] if len(entry) > 2 else True)

        board.drain(1.0)
        out = board.transcript()

        print("\nChecking results:")
        check("Hello World!" in out, "hello prints Hello World!", out)
        check("hello.txt" in out and "fox.txt" in out,
              "ls lists the initramfs files", out)
        check("Hello from the initial ramdisk!" in out,
              "cat reads a file out of the cpio archive", out)
        check("A file in a subdirectory." in out,
              "cat reads a nested file", out)
        check("no such file" in out, "cat reports a missing file", out)
        check("allocated 64 bytes at" in out, "simple_malloc returns a block", out)
        check(out.count("allocated") == 2, "a second allocation also succeeds", out)

        # The two allocations must not overlap: 64 bytes rounded up to the
        # 16-byte grain means the second block starts 64 bytes after the first.
        addresses = [int(line.split("at ")[1].split()[0], 16)
                     for line in out.splitlines() if "allocated" in line and "at " in line]
        check(len(addresses) == 2 and addresses[1] - addresses[0] == 64,
              f"allocations are contiguous and non-overlapping {addresses}", out)

        check("devicetree base: 0x" in out, "devicetree was found via x0", out)
        check("linux,initrd-start" in out,
              "fdt_traverse found /chosen/linux,initrd-start", out)
        check("valid cpio archive" in out,
              "initramfs address from the devicetree holds a real archive", out)
        check("memory: base" in out, "info reads the memory node from the devicetree", out)
        check("Unknown command" not in out, "no command was rejected as unknown", out)

        # --- lab 3: exceptions ---
        print("\n  -- exceptions --")
        check("running at EL1" in out, "kernel dropped from EL2 to EL1", out)
        check(out.count("exception taken from EL0") == 5,
              "user program at EL0 trapped through SVC exactly 5 times", out)
        # ESR class 0x15 is "SVC from AArch64": 0x15 << 26 == 0x54000000, and
        # bit 25 (IL, 32-bit instruction) makes 0x56000000.
        check("esr_el1 : 0x0000000056000000" in out,
              "esr_el1 decodes to EC 0x15, an AArch64 SVC", out)
        check("spsr_el1: 0x0000000000000340" in out,
              "spsr shows the trap came from EL0t with IRQ unmasked", out)
        check("user program exited" in out and "returned to the kernel, now at EL1" in out,
              "kernel resumed cleanly after the user program exited", out)
        # The EL0 program leaves x19 clobbered, and leave_el0 returns straight
        # into C rather than through eret. If the callee-saved registers are not
        # restored, the shell resumes with its own state corrupted and every
        # later command is mangled.
        after_exc = out.split("returned to the kernel, now at EL1", 1)
        check(len(after_exc) == 2 and "Unknown command" not in after_exc[1],
              "the shell's registers survived the excursion to EL0", out)

        # --- lab 3: timers ---
        print("\n  -- timers --")
        # Asserting a fixed name order would be wrong: which timeout expires
        # first depends on when each command was typed, and the host's timing
        # is not controlled. The real invariants are that each one waits the
        # delay it was given, and that they are dispatched in expiry order.
        fired = re.findall(
            r"\[timeout\] (\w+)\s+\(registered at ([\d.]+)s, "
            r"fired at ([\d.]+)s, waited ([\d.]+)s\)", out)
        wanted = {"first": 2.0, "second": 4.0, "third": 6.0}

        check(len(fired) == 3 and {n for n, _, _, _ in fired} == set(wanted),
              f"all three timeouts fired {[n for n, _, _, _ in fired]}", out)

        accurate = all(abs(float(waited) - wanted[name]) < 0.25
                       for name, _, _, waited in fired)
        check(accurate,
              "each timeout waited the requested delay "
              f"{[f'{n}: asked {wanted[n]:.0f}s, waited {w}s' for n, _, _, w in fired]}",
              out)

        fire_times = [float(f) for _, _, f, _ in fired]
        check(fire_times == sorted(fire_times),
              f"timeouts were dispatched in expiry order, not registration "
              f"order {fire_times}", out)

        due = [(round(float(reg) + wanted[n], 3), round(float(f), 3))
               for n, reg, f, _ in fired]
        check(all(abs(exp - act) < 0.25 for exp, act in due),
              f"each timeout fired when it was due (expected, actual) {due}", out)

        check(out.count("since boot") >= 2,
              "the periodic report reprogrammed itself and fired repeatedly", out)

        # --- lab 3: async I/O and task preemption ---
        print("\n  -- async I/O and tasks --")
        check("was preempted: yes" in out,
              "a priority-0 timer task preempted a running priority-9 task", out)

        irq_counts = re.findall(r"uart interrupts : (\d+)", out)
        check(bool(irq_counts) and int(irq_counts[-1]) > 0,
              f"the UART is interrupt-driven, not polled (interrupts={irq_counts})", out)

        sent = re.findall(r"bytes sent      : (\d+)", out)
        check(bool(sent) and int(sent[-1]) > 0,
              f"output went out through the TX ring buffer (bytes={sent})", out)

        # --- lab 4: startup allocation and reserved memory ---
        print("\n  -- memory reservation --")
        for region in ("spin tables", "kernel stack", "kernel image",
                       "startup allocator", "devicetree blob", "initramfs"):
            check(region in out, f"startup reserved the {region}", out)

        frames = re.search(r"frames: (\d+) total, (\d+) free, (\d+) reserved", out)
        check(frames is not None, "the buddy system reported its frame counts", out)
        total, free_f, reserved = (int(g) for g in frames.groups())
        check(total - reserved == free_f,
              f"every frame is accounted for: {total} total - {reserved} reserved "
              f"== {free_f} free", out)
        check(reserved > 0, f"critical regions are actually held back ({reserved} frames)", out)

        # --- lab 4: buddy split and merge ---
        print("\n  -- buddy system --")
        splits = re.findall(r"split, released buddy 0x(\w+) order (\d+)", out)
        merges = re.findall(r"merge 0x\w+ \+ 0x\w+ -> order (\d+)", out)
        check(len(splits) >= 5,
              f"a large block was split down to size, releasing each buddy "
              f"({len(splits)} splits)", out)
        check([int(o) for _, o in splits[:5]] == [4, 3, 2, 1, 0],
              f"splits step down one order at a time {[int(o) for _, o in splits[:5]]}", out)
        check(len(merges) >= 5, f"freed buddies coalesced back up ({len(merges)} merges)", out)
        check([int(o) for o in merges[:5]] == [1, 2, 3, 4, 5],
              f"merges step up one order at a time {[int(o) for o in merges[:5]]}", out)

        # --- lab 4: dynamic allocator ---
        print("\n  -- dynamic allocator --")
        check("24-byte chunks share a frame: yes" in out,
              "same-bin allocations are cut from one page frame", out)
        check("100-byte chunk uses a different bin: yes" in out,
              "a larger request lands in a different bin", out)
        check("is empty, returning it to the buddy system" in out,
              "a pool frame goes back to the buddy system once fully freed", out)
        check("leaked 0 frames" in out,
              "the whole allocator demo leaked nothing", out)
        check(re.search(r"got 0x\w+\s+16 KiB", out) is not None,
              "palloc handed out a 2^2-frame block", out)

        # --- lab 5: threads and scheduling ---
        print("\n  -- threads --")
        check("<- current" in out, "ps reports the running thread", out)
        iters = re.findall(r"thread (\d+) iteration (\d+)", out)
        tids = sorted(set(int(t) for t, _ in iters))
        check(len(tids) == 3, f"three kernel threads ran {tids}", out)
        check(len(iters) == 15, f"each ran all five iterations ({len(iters)} lines)", out)
        # Round robin: the threads take turns rather than running to completion.
        check(len(set(t for t, _ in iters[:3])) == 3,
              f"the scheduler round-robins between them {[t for t, _ in iters[:3]]}", out)
        # After they exit, the idle thread reaps them.
        tail = out.split("kthread 3", 1)[-1]
        check(tail.count("<- current") >= 1 and "  thread 2 iteration 4" in tail,
              "threads ran to completion and were reaped", out)

        # --- lab 5: user processes, exec and fork ---
        print("\n  -- user processes --")
        check("user: running at EL0" in out, "exec started a program at EL0", out)
        forked = re.search(r"user: parent forked child pid=(\d+)", out)
        started = re.search(r"user: child started, pid=(\d+)", out)
        check(forked is not None and started is not None and
              forked.group(1) == started.group(1),
              "fork returned the child pid to the parent and 0 to the child", out)
        check("user: parent exiting" in out and "user: child exiting" in out,
              "both processes reached their own exit", out)
        # Interleaved ticks are the timer preempting one user process for the other.
        ticks = re.findall(r"user: (parent|child) tick (\d+)", out)
        who = [w for w, _ in ticks]
        check(len(ticks) == 6 and "parent" in who and "child" in who and
              who != ["parent"] * 3 + ["child"] * 3,
              f"the timer preempted between the two processes {who}", out)

        # --- lab 5: signals ---
        print("\n  -- signals --")
        check("user: handler ran" in out, "a registered handler ran in user mode", out)
        check("user: resumed after the handler" in out,
              "sigreturn restored the context the signal interrupted", out)
        check("killed by SIGKILL" in out,
              "an unhandled SIGKILL terminated the process", out)
        check("THIS SHOULD NOT PRINT" not in out,
              "the killed process really stopped running", out)

        # --- lab 5: mailbox ---
        check(re.search(r"board revision: 0x\w+", out) is not None,
              "mbox_call reached the VideoCore mailbox", out)

        # --- lab 6: the kernel's own address space ---
        print("\n  -- kernel virtual memory --")
        check("mmu on, kernel at 0xffff" in out,
              "the kernel runs translated, from the upper half", out)
        check("boot page tables" in out,
              "startup reserved the tables the kernel booted through", out)
        # Peripherals are reached through the kernel mapping now, so a working
        # mailbox and timer prove device memory is still device memory.
        check("arm memory    : base" in out,
              "MMIO still works through the linear mapping", out)

        # --- lab 6: one address space per process ---
        print("\n  -- user address spaces --")
        # Each `vm` lists every process alive at that moment. Comparing within
        # one report and not across the run is the point: a page table freed by
        # one process is a frame like any other, and the next process is
        # entitled to get it back.
        reports = [dict(re.findall(r"pid (\d+)  pgd: 0x(\w+)", chunk))
                   for chunk in out.split("live address spaces:")[1:]]
        reports = [r for r in reports if r]

        check(any(len(r) >= 2 for r in reports),
              f"a report caught two processes alive at once {reports}", out)
        check(all(len(set(r.values())) == len(r) for r in reports),
              f"processes running together have separate page tables {reports}", out)
        check(out.count("0x0000000000000000  0x0000000000001000  rwx") >= 2,
              "both are loaded at address 0, in their own address space", out)
        check(out.count("0x0000ffffffffb000  0x0000fffffffff000  rw-") >= 2,
              "both have a stack at the top of the user half", out)

        # --- lab 6: demand paging ---
        print("\n  -- demand paging --")
        check(re.search(r"0x0000ffffffffb000  0x0000fffffffff000  rw-   1/4 page", out)
              is not None,
              "only the stack page actually touched has a frame behind it", out)
        faults = re.findall(r"\[Translation fault\]: 0x(\w+)", out)
        check("0000000000000000" in faults,
              "the program was faulted in by its own first instruction fetch", out)
        check(len(faults) >= 6, f"pages arrived as they were used ({len(faults)} faults)", out)

        # A syscall dereferences the user pointer it was handed, so the kernel
        # itself can be the one to take the fault -- and has to resume from it.
        untouched = re.search(r"handing the kernel an untouched page at 0x(\w+)", out)
        check(untouched is not None, "the demo handed over an unmapped page", out)
        if untouched:
            between = out.split(untouched.group(0), 1)[1].split(
                "user: kernel resumed", 1)[0]
            check(f"[Translation fault]: 0x{untouched.group(1)}" in between,
                  "the kernel faulted on the user pointer it was given", out)
        check("user: kernel resumed the fault it took reading that page" in out,
              "and carried on from where it faulted", out)

        # --- lab 6: mmap ---
        print("\n  -- mmap --")
        check("user: mmap anonymous -> 0x0000000010000000" in out,
              "mmap picked an address when given none", out)
        check("user: wrote 42, read back 42" in out,
              "a region mapped without MAP_POPULATE arrived on first write", out)
        check("user: populated region holds 7" in out,
              "MAP_POPULATE mapped the whole region up front", out)
        check("user: mmap at a chosen address -> 0x0000000020000000" in out,
              "a page-aligned address nothing else uses is honoured", out)

        # --- lab 6: copy on write ---
        print("\n  -- copy on write --")
        check("user: child wrote 200, child sees 200" in out and
              "user: parent still sees 100" in out,
              "the child's write split a page the fork had shared", out)
        revisions = re.findall(r"user: (parent|child) read board revision 0x(\w+)", out)
        check(len(revisions) == 2 and revisions[0][1] == revisions[1][1],
              f"mbox_call works from both sides of a fork {revisions}", out)

        # --- lab 6: access violations ---
        print("\n  -- segmentation faults --")
        check("[Segmentation fault]: Kill Process" in out,
              "writing to a read-only region is fatal", out)
        check("user: THIS SHOULD NOT PRINT" not in out,
              "the process really stopped at the faulting instruction", out)

        # --- lab 6: reclaiming an address space ---
        free_counts = [int(n) for n in re.findall(r"frames: \d+ total, (\d+) free", out)]
        check(len(free_counts) >= 3 and free_counts[-1] == free_counts[-2],
              f"every frame the user programs used came back {free_counts[-2:]}", out)

        # --- lab 7: the virtual file system ---
        print("\n  -- virtual file system --")
        check("initramfs/   (mount point)" in out,
              "the initramfs is mounted inside the root filesystem", out)
        check("A file in a subdirectory." in out,
              "a file in a subdirectory of the initramfs reads through the VFS", out)
        check("wrote 14 byte(s)" in out and "vfs-round-trip" in out,
              "a file written to tmpfs reads back", out)
        check("cannot open /initramfs/x.txt" in out,
              "the initramfs refuses to have a file created in it", out)

        # `fs ls ..` was run from /initramfs, which is a mount root: getting
        # back to the root filesystem's listing means the walk crossed out of
        # the mounted filesystem.
        after_cd = out.split("# fs ls ..", 1)
        check(len(after_cd) == 2 and "initramfs/   (mount point)" in after_cd[1][:200],
              "\"..\" out of a mount root crosses back to the filesystem below", out)

        # --- lab 7: file system syscalls ---
        print("\n  -- file system syscalls --")
        check("user: writing this through fd 1, which is /dev/uart" in out,
              "the kernel opened /dev/uart on stdin, stdout and stderr", out)
        check("user: open for writing gave fd 3" in out,
              "the first file a process opens lands after the three it starts with", out)
        check("user: read it back: hello from a user process" in out,
              "a user process wrote a file and read it back", out)
        check("user: the same file by relative path, fd 3" in out,
              "chdir made a relative path resolve", out)
        check("user: mount tmpfs on /home/mnt returned 0" in out and
              "user: a file in the mounted filesystem, fd 3" in out,
              "a process mounted a filesystem and used it", out)
        check("user: after seeking to 6: from the initi" in out,
              "lseek64 moved the read position", out)
        check("user: creating a file in the initramfs returned -1" in out,
              "a read-only filesystem says so to user space too", out)

        # --- lab 7: the framebuffer ---
        print("\n  -- framebuffer --")
        fb = re.search(r"framebuffer: (\d+)x(\d+), pitch (\d+)", out)
        check(fb is not None, "the mailbox allocated a framebuffer", out)
        if fb:
            check(f"user: framebuffer is {fb.group(1)} wide" in out and
                  f"user: framebuffer pitch is {fb.group(3)} bytes" in out,
                  "ioctl reported the same geometry to the process", out)
        check("user: wrote 4 bytes of pixel to the framebuffer" in out,
              "seeking to a pixel and writing it worked", out)
        check("user: fs demo done" in out, "the demo ran to the end", out)

        # --- lab 8: the SD card and FAT32 ---
        print("\n  -- SD card and FAT32 --")
        check("sd: card ready" in out, "the card initialised", out)
        check(re.search(r"type 0x0*c\s+first block 2048", out) is not None,
              "the partition table names a FAT32 partition at block 2048", out)
        check("root cluster 2, 512 bytes per cluster" in out,
              "the boot sector parsed", out)
        check("FAT_R.TXT" in out, "the root directory of the card listed", out)
        check("Hello from the SD card, read through FAT32." in out,
              "a file on the card read through the VFS", out)

        # --- lab 8: writing, and the cache ---
        print("\n  -- write-back cache --")
        check("user: read 44 bytes from /boot/FAT_R.TXT" in out,
              "a user process read the card through the file descriptor calls", out)
        check("user: wrote 19 bytes to /boot/FAT_WS.TXT" in out,
              "a user process created a file on the card", out)
        synced = re.search(r"user: sync wrote (\d+) block\(s\) to the card", out)
        check(synced is not None and int(synced.group(1)) > 0,
              f"sync pushed the dirty blocks out {synced.group(1) if synced else None}", out)
        check("user: reading it back anyway: written but never synced" in out,
              "an unsynced write is still visible to the process that made it", out)
        check("user: fat demo done" in out, "the demo ran to the end", out)

        print("\n--- full transcript " + "-" * 45)
        print(out)
        print("-" * 65)
        # The card outlives the run, so what actually landed on it can be read
        # back here -- which is the only way to tell a real write from a
        # convincing message about one.
        print("\n  -- what reached the card --")
        board.kill()

        listing = subprocess.run(
            [sys.executable, "tools/make_sdcard.py", "ls", SD_IMAGE],
            capture_output=True, text=True).stdout
        print(listing.rstrip())

        check("FAT_WS.TXT" in listing,
              "the synced file is on the card after the run", listing)
        check("FAT_W.TXT" not in listing,
              "the file written after the last sync never reached it", listing)

        synced_text = subprocess.run(
            [sys.executable, "tools/make_sdcard.py", "cat", SD_IMAGE, "FAT_WS.TXT"],
            capture_output=True, text=True).stdout
        check(synced_text == "written and synced\n",
              f"and holds what was written to it ({synced_text!r})", listing)

        consistent = subprocess.run(
            [sys.executable, "tools/make_sdcard.py", "check", SD_IMAGE],
            capture_output=True, text=True)
        check(consistent.returncode == 0,
              f"the file system the kernel left behind is consistent "
              f"{consistent.stdout.strip()}", consistent.stdout)

        # --- lab 8: a card that cannot be trusted ---
        #
        # Every number in a FAT volume decides where a later read or write
        # lands, so a broken one has to be refused rather than followed. Two
        # extra boots, because each needs its own card.
        print("\n  -- malformed volumes --")

        out2, unchanged = run_on_corrupt_card(kernel_path, "num-fats", [], 6)
        check("fat32: the number of allocation tables is implausible" in out2,
              "a boot sector claiming no allocation tables is refused", out2)
        check("could not mount the SD card" in out2,
              "and the volume is not mounted", out2)
        check("Welcome to my-os shell" in out2,
              "while the kernel carries on booting", out2)
        check(unchanged, "nothing was written to the card", out2)

        out3, unchanged = run_on_corrupt_card(
            kernel_path, "cyclic-chain",
            ["fs ls /boot", "fs write /boot/NEW.TXT x", "sync"], 10)
        check(out3.count("a cluster chain leads back into itself") >= 2,
              "a directory whose chain loops is caught, on listing and on "
              "creating", out3)
        check("cannot open /boot/NEW.TXT" in out3,
              "and the operation fails instead of hanging", out3)
        check(unchanged, "nothing was written to the card", out3)

        print("\nAll checks passed.")

    finally:
        board.kill()


if __name__ == "__main__":
    main()

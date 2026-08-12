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

BOOTLOADER = "bootloader/build/bootloader.img"
KERNEL = "build/kernel8.img"
INITRAMFS = "initramfs.cpio"
DTB = "bcm2710-rpi-3-b-plus.dtb"


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

    def drain(self, seconds):
        deadline = time.time() + seconds
        while time.time() < deadline:
            self._pump(max(0.0, deadline - time.time()))

    def transcript(self):
        return self.buf.decode(errors="replace")

    def kill(self):
        self.proc.kill()
        self.proc.wait(timeout=5)


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

        # (command, seconds to wait afterwards). The lab 3 commands need real
        # time to pass, because that is what they are demonstrating.
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
        ]

        print("\nDriving the shell:")
        for command, wait in script:
            board.send_line(command)
            board.drain(wait)

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

        print("\n--- full transcript " + "-" * 45)
        print(out)
        print("-" * 65)
        print("\nAll checks passed.")

    finally:
        board.kill()


if __name__ == "__main__":
    main()

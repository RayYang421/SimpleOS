#!/usr/bin/env python3
"""End-to-end test: boot the bootloader in QEMU, send the kernel over the
emulated UART, then drive the shell and check what comes back.

Usage:
    tools/test_bootloader.py [kernel.img]

Exits non-zero if any check fails, so it works as a regression test.
"""

import os
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

        print("\nDriving the shell:")
        for command in ["help", "hello", "ls", "cat hello.txt",
                        "cat dir/nested.txt", "cat nope.txt",
                        "malloc 64", "malloc 32", "dtb", "info"]:
            board.send_line(command)
            board.drain(0.6)

        board.drain(1.5)
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

        print("\n--- full transcript " + "-" * 45)
        print(out)
        print("-" * 65)
        print("\nAll checks passed.")

    finally:
        board.kill()


if __name__ == "__main__":
    main()

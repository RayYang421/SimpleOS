#!/usr/bin/env python3
"""Send a kernel image to the UART bootloader.

Usage:
    tools/send_kernel.py <tty> <kernel.img>

<tty> is a real serial port (/dev/ttyUSB0) or the pseudo-terminal QEMU prints
when started with `-serial pty` (see `make run-pty`).

Wire protocol -- see bootloader/src/main.c:

    host -> board   "OSCK"                     magic, resynchronisable
    host -> board   uint32 le  size            kernel image length in bytes
    host -> board   uint32 le  checksum        sum of all image bytes, mod 2^32
    board -> host   0x06 ACK / 0x15 NAK        header accepted?
    host -> board   <size bytes>               the image, streamed
    board -> host   0x06 ACK / 0x15 NAK        checksum matched?

The board is always ready for the next byte at 115200 baud, so the image is
streamed rather than acknowledged byte by byte -- the checksum is what catches
a corrupted transfer.
"""

import os
import select
import struct
import sys
import termios
import time
import tty

MAGIC = b"OSCK"
ACK = 0x06
NAK = 0x15

HEADER_TIMEOUT_S = 10.0
FINAL_TIMEOUT_S = 30.0


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY)

    # Raw mode: the line discipline would otherwise mangle control bytes in the
    # binary image (CR/LF translation, XON/XOFF, echo).
    tty.setraw(fd)

    # A real serial adapter also needs the line speed set; a pty has no baud
    # rate and rejects this, which is harmless to skip.
    try:
        attrs = termios.tcgetattr(fd)
        attrs[4] = attrs[5] = termios.B115200  # ispeed, ospeed
        termios.tcsetattr(fd, termios.TCSANOW, attrs)
    except termios.error:
        pass

    return fd


def wait_for_status(fd, timeout, what):
    """Read until ACK or NAK arrives, echoing any banner text on the way.

    The board interleaves human-readable messages with the protocol bytes, but
    ACK/NAK are control codes that never appear in that text, so scanning for
    them is unambiguous.
    """
    deadline = time.time() + timeout
    noise = bytearray()

    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], deadline - time.time())
        if not ready:
            break

        chunk = os.read(fd, 4096)
        if not chunk:
            continue

        for byte in chunk:
            if byte in (ACK, NAK):
                if noise:
                    sys.stdout.write(noise.decode(errors="replace"))
                    sys.stdout.flush()
                if byte == NAK:
                    # The reason follows the NAK as text; give it a moment.
                    time.sleep(0.2)
                    ready, _, _ = select.select([fd], [], [], 0.5)
                    if ready:
                        sys.stdout.write(os.read(fd, 4096).decode(errors="replace"))
                    raise SystemExit(f"Bootloader rejected the {what} (NAK).")
                return
            noise.append(byte)

    raise SystemExit(
        f"No response to the {what} after {timeout:.0f}s. "
        f"Is the bootloader running and waiting for a kernel?"
    )


def main():
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} <tty> <kernel.img>")

    tty_path, kernel_path = sys.argv[1], sys.argv[2]

    with open(kernel_path, "rb") as f:
        image = f.read()
    if not image:
        raise SystemExit(f"{kernel_path} is empty.")

    checksum = sum(image) & 0xFFFFFFFF
    print(f"Kernel: {kernel_path} ({len(image)} bytes, checksum 0x{checksum:08x})")

    fd = open_port(tty_path)

    # Drop anything already queued, so a leftover banner is not mistaken for a
    # reply to the header we are about to send.
    termios.tcflush(fd, termios.TCIFLUSH)

    header = MAGIC + struct.pack("<II", len(image), checksum)
    os.write(fd, header)
    wait_for_status(fd, HEADER_TIMEOUT_S, "header")

    start = time.time()
    sent = 0
    # Chunked purely so progress can be reported; the board imposes no pacing.
    for offset in range(0, len(image), 4096):
        chunk = image[offset:offset + 4096]
        while chunk:
            written = os.write(fd, chunk)
            chunk = chunk[written:]
            sent += written
        print(f"  {sent}/{len(image)} bytes sent...", end="\r", flush=True)

    wait_for_status(fd, FINAL_TIMEOUT_S, "image")
    print(f"\nKernel accepted in {time.time() - start:.2f}s. Board is booting it.")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
import sys, time, struct

tty_path, kernel_path = sys.argv[1], sys.argv[2]
with open(kernel_path, "rb") as f:
    data = f.read()
print(f"Kernel size: {len(data)} bytes")

with open(tty_path, "wb", buffering=0) as tty:
    tty.write(struct.pack("<I", len(data)))
    time.sleep(0.2)
    for i in range(0, len(data), 32):
        tty.write(data[i:i+32])
        tty.flush()
        time.sleep(0.003)
print("Kernel sent.")
import sys, time, struct

tty_path, kernel_path = sys.argv[1], sys.argv[2]
with open(kernel_path, "rb") as f:
    data = f.read()
print(f"Kernel size: {len(data)} bytes")

with open(tty_path, "wb", buffering=0) as tty:
    tty.write(struct.pack("<I", len(data)))
    time.sleep(0.2)
    for i in range(0, len(data), 32):
        tty.write(data[i:i+32])
        tty.flush()
        time.sleep(0.003)
print("Kernel sent.")
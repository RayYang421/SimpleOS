#!/usr/bin/env python3
import subprocess, time, os, select, struct, sys

kernel = sys.argv[1] if len(sys.argv) > 1 else "build/kernel8.img"

proc = subprocess.Popen(
    ["qemu-system-aarch64", "-M", "raspi3b",
     "-kernel", "bootloader/build/bootloader.img",
     "-display", "none", "-serial", "null", "-serial", "stdio"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
os.set_blocking(proc.stdout.fileno(), False)

def read(t=2.0):
    out = b""; end = time.time() + t
    while time.time() < end:
        r, _, _ = select.select([proc.stdout], [], [], 0.05)
        if r:
            try:
                d = proc.stdout.read()
                if d: out += d
            except: pass
    return out

print(read(1.0).decode(errors="ignore"), end="")

with open(kernel, "rb") as f:
    data = f.read()
print(f"\n[Sending {len(data)} bytes...]")
proc.stdin.write(struct.pack("<I", len(data))); proc.stdin.flush()
time.sleep(0.3)
for b in data:
    proc.stdin.write(bytes([b])); proc.stdin.flush()
    time.sleep(0.003)

print(read(5.0).decode(errors="ignore"), end="")
proc.kill()

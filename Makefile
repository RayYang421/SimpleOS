CROSS   = aarch64-linux-gnu
CC      = $(CROSS)-gcc
LD      = $(CROSS)-ld
OBJCOPY = $(CROSS)-objcopy

SRCDIR   = src
BUILDDIR = build
INCLUDE  = include

CFLAGS  = -Wall -O2 -ffreestanding -nostdinc -nostdlib -nostartfiles -I$(INCLUDE)
# Bare-metal images have no separate loadable segments; the warning is noise.
LDFLAGS = --no-warn-rwx-segments

ASM_SRC = $(shell find $(SRCDIR) -name '*.S')
C_SRC   = $(shell find $(SRCDIR) -name '*.c')
OBJS    = $(patsubst $(SRCDIR)/%.S,$(BUILDDIR)/%_s.o,$(ASM_SRC)) \
          $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(C_SRC))

KERNEL     = $(BUILDDIR)/kernel8.img
BOOTLOADER = bootloader/build/bootloader.img
INITRAMFS  = initramfs.cpio
ROOTFS     = rootfs
DTB        = bcm2710-rpi-3-b-plus.dtb
DTB_URL    = https://raw.githubusercontent.com/raspberrypi/firmware/master/boot/$(DTB)

QEMU       = qemu-system-aarch64
QEMU_FLAGS = -M raspi3b -display none -serial null -serial stdio \
             -initrd $(INITRAMFS) -dtb $(DTB)

all: $(KERNEL) $(INITRAMFS)

$(BUILDDIR)/%_s.o: $(SRCDIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/kernel8.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -T linker.ld -o $@ $(OBJS)

$(KERNEL): $(BUILDDIR)/kernel8.elf
	$(OBJCOPY) -O binary $< $@

# --- initial ramdisk ---------------------------------------------------------
# Rebuilt whenever anything under rootfs/ changes. `find .` is what produces the
# leading "./" on every archived name, which the cpio parser accounts for.
$(INITRAMFS): $(shell find $(ROOTFS) -type f 2>/dev/null)
	cd $(ROOTFS) && find . | cpio -o -H newc > ../$(INITRAMFS)

# --- devicetree --------------------------------------------------------------
# Not committed: fetched on demand from the Raspberry Pi firmware repository.
$(DTB):
	curl -sSL -o $@ $(DTB_URL)

# --- bootloader --------------------------------------------------------------
bootloader $(BOOTLOADER):
	$(MAKE) -C bootloader

# --- running -----------------------------------------------------------------
# Boots the kernel directly, the quick path while working on kernel code.
run: $(KERNEL) $(INITRAMFS) $(DTB)
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL)

# Boots the bootloader instead, then waits for a kernel over UART. Use
# `make send` from another terminal, or `make test` to drive both automatically.
run-bootloader: $(BOOTLOADER) $(INITRAMFS) $(DTB)
	$(QEMU) $(QEMU_FLAGS) -kernel $(BOOTLOADER)

# Exposes the emulated UART as a pty so send_kernel.py can drive it, matching
# how a real board is flashed over a USB serial adapter.
run-pty: $(BOOTLOADER) $(INITRAMFS) $(DTB)
	$(QEMU) -M raspi3b -display none -serial null -serial pty \
	        -initrd $(INITRAMFS) -dtb $(DTB) -kernel $(BOOTLOADER)

debug: $(KERNEL) $(INITRAMFS) $(DTB)
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL) -S -s

# End-to-end: boots the bootloader in QEMU, sends the kernel, runs shell commands.
test: $(KERNEL) $(BOOTLOADER) $(INITRAMFS) $(DTB)
	python3 tools/test_bootloader.py

# Sends the kernel to a board (or pty) that is already running the bootloader:
#   make send PORT=/dev/ttyUSB0
PORT ?= /dev/ttyUSB0
send: $(KERNEL)
	python3 tools/send_kernel.py $(PORT) $(KERNEL)

clean:
	rm -rf $(BUILDDIR) $(INITRAMFS)
	$(MAKE) -C bootloader clean

.PHONY: all bootloader run run-bootloader run-pty debug test send clean

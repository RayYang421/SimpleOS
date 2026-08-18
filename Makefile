CROSS   = aarch64-linux-gnu
CC      = $(CROSS)-gcc
LD      = $(CROSS)-ld
OBJCOPY = $(CROSS)-objcopy

SRCDIR   = src
BUILDDIR = build
INCLUDE  = include

# -mgeneral-regs-only keeps the compiler off the FP/SIMD registers. They are
# trapped at EL1 by default, and GCC will happily vectorise something like a
# large struct memset into SIMD stores, which faults. Forbidding them also
# means a context switch never has to save them.
# MMU_ENABLED puts peripheral registers behind the kernel's linear mapping. The
# bootloader compiles the same drivers without it, since it runs with
# translation off.
CFLAGS  = -Wall -O2 -ffreestanding -nostdinc -nostdlib -nostartfiles -mgeneral-regs-only \
          -DMMU_ENABLED -I$(INCLUDE) -MMD -MP $(EXTRA_CFLAGS)
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

# User programs, built as flat binaries and dropped into the initramfs for
# exec to load. Linked at 0 and position-independent, so the kernel can put
# them wherever it finds room.
USER_SRC   = $(wildcard user/*.c)
USER_BIN   = $(patsubst user/%.c,$(ROOTFS)/%.img,$(USER_SRC))
DTB        = bcm2710-rpi-3-b-plus.dtb
DTB_URL    = https://raw.githubusercontent.com/raspberrypi/firmware/master/boot/$(DTB)

# The SD card the FAT32 exercises run against: an MBR with one FAT32 partition
# at block 2048, holding the file the read test looks for. Built rather than
# committed, so a run always starts from a known card.
SDIMAGE    = sd.img

QEMU       = qemu-system-aarch64
# raspi3b always emulates four cores, and QEMU holds the three it is not
# booting in a spin-table stub that never halts. Those spinning cores starve
# QEMU's own timer delivery, so the guest sees core-timer interrupts arrive up
# to a second after their deadline -- entirely outside the kernel's control.
#
# thread=single stops them from burning a host thread each, and icount drives
# the guest clock from instructions retired rather than host wall time, which
# makes timer delivery exact instead of dependent on how loaded the host is.
# Without these, the timing checks in tools/test_bootloader.py flake.
QEMU_ACCEL = -accel tcg,thread=single -icount shift=auto,sleep=on
QEMU_FLAGS = -M raspi3b $(QEMU_ACCEL) -display none -serial null -serial stdio \
             -initrd $(INITRAMFS) -dtb $(DTB) \
             -drive if=sd,file=$(SDIMAGE),format=raw

all: $(KERNEL) $(INITRAMFS) $(SDIMAGE)

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
$(ROOTFS)/%.img: user/%.c user/user.ld
	@mkdir -p $(BUILDDIR)/user
	$(CC) $(CFLAGS) -c $< -o $(BUILDDIR)/user/$*.o
	$(LD) $(LDFLAGS) -T user/user.ld -o $(BUILDDIR)/user/$*.elf $(BUILDDIR)/user/$*.o
	$(OBJCOPY) -O binary $(BUILDDIR)/user/$*.elf $@

$(INITRAMFS): $(USER_BIN) $(shell find $(ROOTFS) -type f 2>/dev/null)
	cd $(ROOTFS) && find . | cpio -o -H newc > ../$(INITRAMFS)

# --- SD card -----------------------------------------------------------------
# Needs sfdisk and mkfs.vfat, but no root: everything is written into a plain
# file. `make sdcard` starts a fresh one, which is how a test gets a card with
# no leftovers from the last run.
$(SDIMAGE):
	python3 tools/make_sdcard.py build $@

sdcard:
	rm -f $(SDIMAGE)
	$(MAKE) $(SDIMAGE)

# --- devicetree --------------------------------------------------------------
# Not committed: fetched on demand from the Raspberry Pi firmware repository.
$(DTB):
	curl -sSL -o $@ $(DTB_URL)

# --- bootloader --------------------------------------------------------------
bootloader $(BOOTLOADER):
	$(MAKE) -C bootloader

# --- running -----------------------------------------------------------------
# Boots the kernel directly, the quick path while working on kernel code.
run: $(KERNEL) $(INITRAMFS) $(DTB) $(SDIMAGE)
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL)

# Boots the bootloader instead, then waits for a kernel over UART. Use
# `make send` from another terminal, or `make test` to drive both automatically.
run-bootloader: $(BOOTLOADER) $(INITRAMFS) $(DTB) $(SDIMAGE)
	$(QEMU) $(QEMU_FLAGS) -kernel $(BOOTLOADER)

# Exposes the emulated UART as a pty so send_kernel.py can drive it, matching
# how a real board is flashed over a USB serial adapter.
run-pty: $(BOOTLOADER) $(INITRAMFS) $(DTB) $(SDIMAGE)
	$(QEMU) -M raspi3b $(QEMU_ACCEL) -display none -serial null -serial pty \
	        -initrd $(INITRAMFS) -dtb $(DTB) \
	        -drive if=sd,file=$(SDIMAGE),format=raw -kernel $(BOOTLOADER)

debug: $(KERNEL) $(INITRAMFS) $(DTB) $(SDIMAGE)
	$(QEMU) $(QEMU_FLAGS) -kernel $(KERNEL) -S -s

# End-to-end: boots the bootloader in QEMU, sends the kernel, runs shell commands.
test: $(KERNEL) $(BOOTLOADER) $(INITRAMFS) $(DTB) $(SDIMAGE)
	python3 tools/test_bootloader.py

# Sends the kernel to a board (or pty) that is already running the bootloader:
#   make send PORT=/dev/ttyUSB0
PORT ?= /dev/ttyUSB0
send: $(KERNEL)
	python3 tools/send_kernel.py $(PORT) $(KERNEL)

clean:
	rm -rf $(BUILDDIR) $(INITRAMFS) $(USER_BIN) sd-corrupt.img
	$(MAKE) -C bootloader clean

.PHONY: all bootloader sdcard run run-bootloader run-pty debug test send clean

# -MMD -MP above writes a .d file beside each object listing the headers it
# used, and including them is what makes a header change rebuild what depends
# on it. Without this a stale object survives a header edit, which shows up as
# a working build behaving as though the edit never happened.
#
# It goes last: an included file's first rule would otherwise become the
# default goal, and plain `make` would build one object instead of everything.
DEPS = $(OBJS:.o=.d)
-include $(DEPS)

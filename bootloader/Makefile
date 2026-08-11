CROSS   = aarch64-linux-gnu
CC      = $(CROSS)-gcc
LD      = $(CROSS)-ld
OBJCOPY = $(CROSS)-objcopy

SRCDIR   = src
BUILDDIR = build
INCLUDE  = include

CFLAGS = -Wall -O2 -ffreestanding -nostdinc -nostdlib -nostartfiles -I$(INCLUDE)

ASM_SRC = $(shell find $(SRCDIR) -name '*.S')
C_SRC   = $(shell find $(SRCDIR) -name '*.c')
OBJS    = $(patsubst $(SRCDIR)/%.S,$(BUILDDIR)/%_s.o,$(ASM_SRC)) \
          $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(C_SRC))

all: $(BUILDDIR)/bootloader.img

$(BUILDDIR)/%_s.o: $(SRCDIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/bootloader.elf: $(OBJS) linker.ld
	$(LD) -T linker.ld -o $@ $(OBJS)

$(BUILDDIR)/bootloader.img: $(BUILDDIR)/bootloader.elf
	$(OBJCOPY) -O binary $< $@

run: $(BUILDDIR)/bootloader.img
	qemu-system-aarch64 -M raspi3b -kernel $< -display none -serial null -serial pty

clean:
	rm -rf $(BUILDDIR)/*

.PHONY: all run clean
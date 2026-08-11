CROSS   = aarch64-linux-gnu
CC      = $(CROSS)-gcc
LD      = $(CROSS)-ld
OBJCOPY = $(CROSS)-objcopy

SRCDIR   = src
BUILDDIR = build


ASM_SRC = $(SRCDIR)/boot/start.S
OBJS    = $(BUILDDIR)/start.o

all: $(BUILDDIR)/kernel8.img

$(BUILDDIR)/start.o: $(SRCDIR)/boot/start.S | $(BUILDDIR)
	$(CC) -c $< -o $@

$(BUILDDIR)/kernel8.elf: $(OBJS) linker.ld
	$(LD) -T linker.ld -o $@ $(OBJS)

$(BUILDDIR)/kernel8.img: $(BUILDDIR)/kernel8.elf
	$(OBJCOPY) -O binary $< $@

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

run: $(BUILDDIR)/kernel8.img
	qemu-system-aarch64 -M raspi3b -kernel $(BUILDDIR)/kernel8.img -display none -d in_asm

debug: $(BUILDDIR)/kernel8.img
	qemu-system-aarch64 -M raspi3b -kernel $(BUILDDIR)/kernel8.img -display none -S -s

clean:
	rm -rf $(BUILDDIR)/*

.PHONY: all run debug clean
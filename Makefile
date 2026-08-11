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

all: $(BUILDDIR)/kernel8.img

$(BUILDDIR)/%_s.o: $(SRCDIR)/%.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/kernel8.elf: $(OBJS) linker.ld
	$(LD) -T linker.ld -o $@ $(OBJS)

$(BUILDDIR)/kernel8.img: $(BUILDDIR)/kernel8.elf
	$(OBJCOPY) -O binary $< $@

run: $(BUILDDIR)/kernel8.img
	qemu-system-aarch64 -M raspi3b -kernel $< -display none -serial null -serial stdio

debug: $(BUILDDIR)/kernel8.img
	qemu-system-aarch64 -M raspi3b -kernel $< -display none -serial null -serial stdio -S -s

clean:
	rm -rf $(BUILDDIR)/*

.PHONY: all run debug clean
CROSS   = aarch64-linux-gnu
CC      = $(CROSS)-gcc
LD      = $(CROSS)-ld
OBJCOPY = $(CROSS)-objcopy

SRCDIR   = src
BUILDDIR = build

# The UART driver and the register headers are shared with the kernel rather
# than duplicated here -- two copies of a driver drift apart.
INCLUDE  = ../include
SHARED   = ../src

# -mgeneral-regs-only keeps the compiler off the FP/SIMD registers. They are
# trapped at EL1 by default, and GCC will happily vectorise something like a
# large struct memset into SIMD stores, which faults. Forbidding them also
# means a context switch never has to save them.
CFLAGS  = -Wall -O2 -ffreestanding -nostdinc -nostdlib -nostartfiles -mgeneral-regs-only \
          -I$(INCLUDE) $(EXTRA_CFLAGS)
LDFLAGS = --no-warn-rwx-segments

OBJS = $(BUILDDIR)/start_s.o $(BUILDDIR)/main.o $(BUILDDIR)/uart.o

all: $(BUILDDIR)/bootloader.img

$(BUILDDIR)/start_s.o: $(SRCDIR)/start.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/main.o: $(SRCDIR)/main.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/uart.o: $(SHARED)/drivers/uart.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/bootloader.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -T linker.ld -o $@ $(OBJS)

$(BUILDDIR)/bootloader.img: $(BUILDDIR)/bootloader.elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf $(BUILDDIR)

.PHONY: all clean

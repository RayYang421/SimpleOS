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

CFLAGS  = -Wall -O2 -ffreestanding -nostdinc -nostdlib -nostartfiles -I$(INCLUDE)
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

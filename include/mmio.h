#ifndef MMIO_H
#define MMIO_H

#include "mmu.h"

/* Peripherals are reached through the kernel's linear mapping once the MMU is
 * on. The bootloader links the same drivers with translation off, where a
 * physical address is all there is -- hence the offset rather than a second set
 * of register definitions. */
#ifdef MMU_ENABLED
#define IO_BASE     KERNEL_VA_BASE
#else
#define IO_BASE     0UL
#endif

#define MMIO_BASE       (IO_BASE + 0x3F000000UL)

#define REG(addr)       (*(volatile unsigned int *)(addr))

static inline void mmio_write(unsigned long addr, unsigned int val) { REG(addr) = val; }
static inline unsigned int mmio_read(unsigned long addr)            { return REG(addr); }

#endif

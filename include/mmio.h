#ifndef MMIO_H
#define MMIO_H


#define MMIO_BASE       0x3F000000
 
#define REG(addr)       (*(volatile unsigned int *)(addr))
 
static inline void mmio_write(long addr, unsigned int val) { REG(addr) = val; }
static inline unsigned int mmio_read(long addr)            { return REG(addr); }


#endif
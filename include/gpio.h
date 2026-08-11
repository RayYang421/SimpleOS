#ifndef GPIO_H
#define GPIO_H
#include "mmio.h"
 
#define GPFSEL1     (MMIO_BASE + 0x00200004)
#define GPPUD       (MMIO_BASE + 0x00200094)
#define GPPUDCLK0   (MMIO_BASE + 0x00200098)
 
#endif
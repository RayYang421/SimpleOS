#ifndef GPIO_H
#define GPIO_H
#include "mmio.h"

/* Each GPFSEL register holds the function select for ten pins, three bits
 * each: GPFSEL1 covers 10-19 (the UART pins), GPFSEL4 covers 40-49 and GPFSEL5
 * covers 50-59 (the SD card pins). */
#define GPFSEL1     (MMIO_BASE + 0x00200004)
#define GPFSEL4     (MMIO_BASE + 0x00200010)
#define GPFSEL5     (MMIO_BASE + 0x00200014)

/* Pull-up/down control: write the direction to GPPUD, then clock it into the
 * pins named in GPPUDCLK0 (0-31) or GPPUDCLK1 (32-53). */
#define GPPUD       (MMIO_BASE + 0x00200094)
#define GPPUDCLK0   (MMIO_BASE + 0x00200098)
#define GPPUDCLK1   (MMIO_BASE + 0x0020009C)

#endif

#ifndef SD_H
#define SD_H

#include "types.h"

#define SD_BLOCK_SIZE 512

/* Brings the card up: resets the host controller, negotiates with the card and
 * selects it. Returns 0 on success, -1 if there is no usable card -- which is
 * the normal answer when QEMU was started without -drive if=sd. */
int sd_init(void);

/* 1 once sd_init has succeeded. Everything above the driver checks this rather
 * than failing one block at a time. */
int sd_present(void);

/* One 512-byte block each, addressed by block number. Return 0 on success. */
int readblock(uint32_t lba, void *buf);
int writeblock(uint32_t lba, const void *buf);

/* Blocks moved since boot, for the shell to report. */
void sd_stats(uint64_t *reads, uint64_t *writes);

#endif

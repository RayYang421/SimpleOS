#ifndef BCACHE_H
#define BCACHE_H

#include "types.h"

/* A page cache over the SD card.
 *
 * Everything above the driver goes through this, which is what makes the lab's
 * rule -- nothing reaches the card until sync is called -- a property of the
 * design rather than of remembering to be careful. A miss reads a whole page
 * of blocks, so the block after the one that was asked for is already there.
 *
 * Frames are never evicted while dirty, because evicting one would mean
 * writing it. */

/* The 512 bytes at this block number, loaded if they are not already cached.
 * NULL if there is no card or memory ran out. */
uint8_t *bcache_get(uint32_t lba);

/* Says the caller changed the bytes bcache_get returned. */
void bcache_mark_dirty(uint32_t lba);

/* Writes every dirty block back and returns how many. */
int bcache_sync(void);

void bcache_stats(uint64_t *hits, uint64_t *misses, uint64_t *pages,
                  uint64_t *dirty_blocks);

#endif

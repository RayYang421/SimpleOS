#ifndef PAGE_H
#define PAGE_H

#include "types.h"

#define PAGE_SHIFT 12
#define PAGE_SIZE  (1UL << PAGE_SHIFT)

/* 2^18 frames is 1 GiB, past the top of the Rpi3's usable RAM, so the largest
 * block the buddy system can form is bounded by memory rather than by this. */
#define MAX_ORDER  18

/* Startup, in the order it has to happen:
 *
 *   page_init      claim the frame array from the startup allocator and mark
 *                  every frame unavailable
 *   memory_reserve mark the ranges that must never be handed out
 *   page_finalize  release everything still unreserved into the buddy lists
 *
 * The frame array cannot come from the page allocator it describes, which is
 * why the startup allocator exists. */
void page_init(uint64_t mem_base, uint64_t mem_size);
void memory_reserve(uint64_t start, uint64_t end, const char *label);
void page_finalize(void);

/* 2^order contiguous frames, or NULL. The address is page-aligned. */
void *page_alloc(int order);
void page_free(void *addr);

/* Splits and merges are printed when logging is on, which is how the buddy
 * system's behaviour is meant to be demonstrated. Off during startup, since
 * releasing all of memory one frame at a time would print for a very long
 * time. Returns the previous setting. */
int page_set_log(int on);

void page_report(void);
void page_stats(uint64_t *total_frames, uint64_t *free_frames,
                uint64_t *reserved_frames);

/* Order of the allocated block at addr, or -1 if it is not an allocation. */
int page_block_order(void *addr);

/* Blocks currently free at exactly this order. Lets a caller take every
 * exact-size block without triggering a split. */
int page_order_count(int order);

#endif

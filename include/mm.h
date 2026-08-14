#ifndef MM_H
#define MM_H

#include "types.h"

/* --- startup allocator ---
 *
 * Bump allocator over a fixed heap carved out by the linker script, with no
 * free(). It exists to break a circular dependency: the page allocator needs a
 * frame array before it can hand out anything, and that array has to come from
 * somewhere that does not need the page allocator. */
void *simple_malloc(size_t size);
void simple_malloc_stats(size_t *used, size_t *total);

/* The span the startup allocator owns, so it can be reserved before the buddy
 * system starts handing memory out. */
void startup_alloc_range(uint64_t *start, uint64_t *end);

/* Works out usable RAM, reserves everything that must not be handed out, and
 * starts the buddy system on what is left. */
void mem_init(void);

/* --- dynamic allocator ---
 *
 * Small requests are cut from page frames into fixed-size chunks; anything
 * larger goes straight to the page allocator. Chunks in one page all share the
 * page's address prefix, which is how kfree finds the pool a chunk came from
 * without being told. */
void *kmalloc(size_t size);
void  kfree(void *ptr);

/* Allocations and frees are printed when logging is on. Returns the previous
 * setting. */
int  kmalloc_set_log(int on);
void kmalloc_report(void);

#endif

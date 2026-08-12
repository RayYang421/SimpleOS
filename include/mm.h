#ifndef MM_H
#define MM_H

#include "types.h"

/* Bump allocator for early boot: hands out 16-byte-aligned blocks from a fixed
 * heap carved out by the linker script. There is deliberately no free() --
 * nothing here ever needs reclaiming. */
void *simple_malloc(size_t size);

/* Bytes handed out so far and total heap capacity, for the shell's report. */
void simple_malloc_stats(size_t *used, size_t *total);

#endif

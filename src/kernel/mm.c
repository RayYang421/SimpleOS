#include "mm.h"

/* Carved out by the linker script, past the kernel's .bss. Declared as arrays
 * so the symbol's *address* is the value we want, not its contents. */
extern char __heap_start[];
extern char __heap_end[];

#define HEAP_ALIGN 16UL

static char *brk;   /* next free byte; NULL until the first allocation */

void *simple_malloc(size_t size) {
    if (brk == 0) brk = __heap_start;
    if (size == 0) return 0;

    /* Compare against the remaining space before rounding up, so a huge size
     * is rejected here rather than wrapping around in the alignment step. */
    size_t avail = (size_t)(__heap_end - brk);
    if (size > avail) return 0;

    size_t need = (size + (HEAP_ALIGN - 1)) & ~(HEAP_ALIGN - 1);
    if (need > avail) return 0;

    char *block = brk;
    brk += need;
    return block;
}

void simple_malloc_stats(size_t *used, size_t *total) {
    char *cur = brk ? brk : __heap_start;
    if (used)  *used  = (size_t)(cur - __heap_start);
    if (total) *total = (size_t)(__heap_end - __heap_start);
}

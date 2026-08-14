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

/* The whole heap, not just the part handed out so far: the buddy system must
 * not be given the unused tail, or a later simple_malloc would collide with
 * memory it had already allocated to someone else. */
void startup_alloc_range(uint64_t *start, uint64_t *end) {
    if (start) *start = (uint64_t)(uintptr_t)__heap_start;
    if (end)   *end   = (uint64_t)(uintptr_t)__heap_end;
}

void simple_malloc_stats(size_t *used, size_t *total) {
    char *cur = brk ? brk : __heap_start;
    if (used)  *used  = (size_t)(cur - __heap_start);
    if (total) *total = (size_t)(__heap_end - __heap_start);
}

#include "page.h"
#include "mm.h"
#include "uart.h"
#include "irq.h"

/* Buddy system over a one-entry-per-frame array.
 *
 * frame[i] describes the block *starting* at frame i; interior frames of a
 * block are left as F_BUDDY and never consulted, which is what keeps
 * allocation and free proportional to the order rather than to the block size.
 *
 *    0 .. MAX_ORDER   free block head, this many orders wide
 *    F_BUDDY          not a block head: either interior, or awaiting release
 *    F_RESERVED       never allocable
 *    <= F_ALLOC_BASE  allocated block head, order encoded in the value
 */
#define F_BUDDY      (-1)
#define F_RESERVED   (-2)
#define F_ALLOC_BASE (-3)

static inline int8_t encode_alloc(int order) { return (int8_t)(F_ALLOC_BASE - order); }
static inline int    decode_alloc(int8_t v)  { return (int)(F_ALLOC_BASE - v); }
static inline int    is_alloc(int8_t v)      { return v <= F_ALLOC_BASE; }

/* Free blocks are threaded through the memory they describe -- a free frame is
 * by definition not in use, so its first bytes can hold the list node. That
 * costs no extra metadata and makes removal O(1), which is what gets the whole
 * allocator to O(log n). */
struct free_node {
    struct free_node *next;
    struct free_node *prev;
};

static struct free_node free_list[MAX_ORDER + 1];   /* circular sentinels */
static int8_t  *frame;
static uint64_t base_addr;      /* physical address of frame 0 */
static uint64_t nframes;
static uint64_t reserved_count;
static uint64_t free_count;
static int      log_enabled;

static void list_init(struct free_node *head) {
    head->next = head;
    head->prev = head;
}

static void list_add(struct free_node *head, struct free_node *node) {
    node->next = head->next;
    node->prev = head;
    head->next->prev = node;
    head->next = node;
}

static void list_del(struct free_node *node) {
    node->prev->next = node->next;
    node->next->prev = node->prev;
}

static int list_empty(struct free_node *head) { return head->next == head; }

static inline uint64_t idx_to_addr(uint64_t idx) {
    return base_addr + (idx << PAGE_SHIFT);
}

static inline uint64_t addr_to_idx(uint64_t addr) {
    return (addr - base_addr) >> PAGE_SHIFT;
}

static void log_block(const char *what, uint64_t idx, int order) {
    if (!log_enabled) return;
    uart_puts(what);
    uart_hex(idx_to_addr(idx));
    uart_puts(" order ");
    uart_dec(order);
    uart_puts(" (");
    uart_dec((1UL << order) * (PAGE_SIZE / 1024));
    uart_puts(" KiB)\n");
}

int page_set_log(int on) {
    int was = log_enabled;
    log_enabled = on;
    return was;
}

void page_init(uint64_t mem_base, uint64_t mem_size) {
    base_addr = mem_base;
    nframes   = mem_size >> PAGE_SHIFT;

    for (int i = 0; i <= MAX_ORDER; i++) list_init(&free_list[i]);

    frame = (int8_t *)simple_malloc(nframes);
    if (frame == 0) {
        uart_puts("page_init: startup allocator could not provide the frame "
                  "array; no page allocator\n");
        nframes = 0;
        return;
    }

    /* Nothing is allocable until page_finalize releases it, so reservations
     * made in between simply stay out of the lists. */
    for (uint64_t i = 0; i < nframes; i++) frame[i] = F_BUDDY;
}

void memory_reserve(uint64_t start, uint64_t end, const char *label) {
    if (nframes == 0 || end <= start) return;

    /* Round outwards: a partially covered frame must be reserved whole. */
    if (start < base_addr) start = base_addr;
    uint64_t limit = base_addr + (nframes << PAGE_SHIFT);
    if (end > limit) end = limit;
    if (end <= start) return;

    uint64_t first = addr_to_idx(start & ~(PAGE_SIZE - 1));
    uint64_t last  = addr_to_idx((end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));

    uint64_t n = 0;
    for (uint64_t i = first; i < last && i < nframes; i++) {
        if (frame[i] != F_RESERVED) { frame[i] = F_RESERVED; n++; }
    }
    reserved_count += n;

    uart_puts("  reserve ");
    uart_hex(start);
    uart_puts(" .. ");
    uart_hex(end);
    uart_puts("  ");
    uart_dec(n);
    uart_puts(" new frames  ");
    uart_puts(label ? label : "");
    uart_puts("\n");
}

/* Puts a single free block into the lists, merging with its buddy for as long
 * as the buddy is free and the same size. */
static void free_block(uint64_t idx, int order) {
    while (order < MAX_ORDER) {
        uint64_t buddy = idx ^ (1UL << order);

        if (buddy >= nframes) break;
        if (frame[buddy] != order) break;       /* not a free head of this size */

        /* The merged block must not run past the end of memory. */
        uint64_t merged = (idx < buddy) ? idx : buddy;
        if (merged + (1UL << (order + 1)) > nframes) break;

        list_del((struct free_node *)idx_to_addr(buddy));
        frame[buddy] = F_BUDDY;
        frame[idx]   = F_BUDDY;

        if (log_enabled) {
            uart_puts("    merge ");
            uart_hex(idx_to_addr(idx));
            uart_puts(" + ");
            uart_hex(idx_to_addr(buddy));
            uart_puts(" -> order ");
            uart_dec(order + 1);
            uart_puts("\n");
        }

        idx = merged;
        order++;
    }

    frame[idx] = (int8_t)order;
    list_add(&free_list[order], (struct free_node *)idx_to_addr(idx));
}

void page_finalize(void) {
    int was = page_set_log(0);   /* releasing all of RAM would print forever */

    for (uint64_t i = 0; i < nframes; i++) {
        if (frame[i] == F_BUDDY) {
            free_block(i, 0);
            free_count++;
        }
    }

    page_set_log(was);
}

void *page_alloc(int order) {
    if (order < 0 || order > MAX_ORDER || nframes == 0) return 0;

    uint64_t daif = irq_disable_save();

    int found = order;
    while (found <= MAX_ORDER && list_empty(&free_list[found])) found++;
    if (found > MAX_ORDER) {
        irq_restore(daif);
        return 0;
    }

    struct free_node *node = free_list[found].next;
    list_del(node);
    uint64_t idx = addr_to_idx((uint64_t)node);

    if (log_enabled) {
        uart_puts("  alloc order ");
        uart_dec(order);
        uart_puts(": took ");
        uart_hex(idx_to_addr(idx));
        uart_puts(" from order ");
        uart_dec(found);
        uart_puts("\n");
    }

    /* Hand back the upper half of each split; the lower half carries on down
     * to the size actually asked for. */
    while (found > order) {
        found--;
        uint64_t buddy = idx + (1UL << found);
        frame[buddy] = (int8_t)found;
        list_add(&free_list[found], (struct free_node *)idx_to_addr(buddy));
        log_block("    split, released buddy ", buddy, found);
    }

    frame[idx] = encode_alloc(order);
    free_count -= (1UL << order);

    irq_restore(daif);
    return (void *)idx_to_addr(idx);
}

void page_free(void *addr) {
    if (addr == 0 || nframes == 0) return;

    uint64_t a = (uint64_t)addr;
    if (a < base_addr || a >= base_addr + (nframes << PAGE_SHIFT)) return;
    if (a & (PAGE_SIZE - 1)) return;

    uint64_t daif = irq_disable_save();
    uint64_t idx = addr_to_idx(a);

    if (!is_alloc(frame[idx])) {
        irq_restore(daif);
        uart_puts("page_free: ");
        uart_hex(a);
        uart_puts(" is not an allocated block\n");
        return;
    }

    int order = decode_alloc(frame[idx]);
    if (log_enabled) {
        uart_puts("  free ");
        uart_hex(a);
        uart_puts(" order ");
        uart_dec(order);
        uart_puts("\n");
    }

    frame[idx] = F_BUDDY;
    free_count += (1UL << order);
    free_block(idx, order);

    irq_restore(daif);
}

int page_block_order(void *addr) {
    if (addr == 0 || nframes == 0) return -1;

    uint64_t a = (uint64_t)addr;
    if (a < base_addr || a >= base_addr + (nframes << PAGE_SHIFT)) return -1;
    if (a & (PAGE_SIZE - 1)) return -1;

    int8_t v = frame[addr_to_idx(a)];
    return is_alloc(v) ? decode_alloc(v) : -1;
}

int page_order_count(int order) {
    if (order < 0 || order > MAX_ORDER) return 0;

    uint64_t daif = irq_disable_save();
    int n = 0;
    for (struct free_node *p = free_list[order].next;
         p != &free_list[order]; p = p->next) n++;
    irq_restore(daif);
    return n;
}

void page_stats(uint64_t *total_frames, uint64_t *free_frames,
                uint64_t *reserved_frames) {
    if (total_frames)    *total_frames    = nframes;
    if (free_frames)     *free_frames     = free_count;
    if (reserved_frames) *reserved_frames = reserved_count;
}

void page_report(void) {
    uart_puts("frames: ");
    uart_dec(nframes);
    uart_puts(" total, ");
    uart_dec(free_count);
    uart_puts(" free, ");
    uart_dec(reserved_count);
    uart_puts(" reserved\nfree lists:\n");

    uint64_t daif = irq_disable_save();
    for (int order = 0; order <= MAX_ORDER; order++) {
        uint64_t n = 0;
        for (struct free_node *p = free_list[order].next;
             p != &free_list[order]; p = p->next) n++;
        if (n == 0) continue;

        uart_puts("  order ");
        uart_dec(order);
        uart_puts(":\t");
        uart_dec(n);
        uart_puts(" block(s)\t");
        uart_dec((1UL << order) * (PAGE_SIZE / 1024));
        uart_puts(" KiB each\n");
    }
    irq_restore(daif);
}

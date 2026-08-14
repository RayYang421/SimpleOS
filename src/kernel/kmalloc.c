#include "mm.h"
#include "page.h"
#include "uart.h"
#include "irq.h"

/* Chunk sizes. A request is rounded up to the first bin that fits; anything
 * past the last bin is served by whole page frames instead. */
static const uint32_t bin_size[] = { 16, 32, 48, 64, 96, 128, 256, 512, 1024 };
#define NBINS ((int)(sizeof(bin_size) / sizeof(bin_size[0])))

/* One of these sits at the base of every page used as a pool, which is also
 * why a chunk is never page-aligned -- kfree relies on that to tell a chunk
 * apart from a whole-page allocation. */
struct pool_page {
    struct pool_page *next;
    struct pool_page *prev;
    void    *free_chunks;    /* singly linked through the free chunks */
    uint32_t chunk_size;
    uint32_t total;
    uint32_t used;
    int      bin;
};

#define HEADER_SIZE ((sizeof(struct pool_page) + 15UL) & ~15UL)

/* Pages of each bin that still have a chunk to give. Full pages are unlinked
 * and relinked when a chunk comes back, so allocation never has to search. */
static struct pool_page *partial[NBINS];

static int log_enabled;
static uint64_t stat_chunk_allocs, stat_chunk_frees;
static uint64_t stat_pool_pages, stat_big_allocs;

int kmalloc_set_log(int on) {
    int was = log_enabled;
    log_enabled = on;
    return was;
}

static int size_to_bin(size_t size) {
    for (int i = 0; i < NBINS; i++) {
        if (size <= bin_size[i]) return i;
    }
    return -1;
}

/* Smallest order whose block covers size bytes. */
static int size_to_order(size_t size) {
    int order = 0;
    uint64_t bytes = PAGE_SIZE;
    while (bytes < size && order < MAX_ORDER) { bytes <<= 1; order++; }
    return bytes >= size ? order : -1;
}

static void unlink_page(struct pool_page *p) {
    if (p->prev) p->prev->next = p->next;
    else         partial[p->bin] = p->next;
    if (p->next) p->next->prev = p->prev;
    p->next = p->prev = 0;
}

static void link_page(struct pool_page *p) {
    p->prev = 0;
    p->next = partial[p->bin];
    if (p->next) p->next->prev = p;
    partial[p->bin] = p;
}

/* Carves a fresh frame into chunks and threads them onto its free list. */
static struct pool_page *new_pool_page(int bin) {
    void *page = page_alloc(0);
    if (page == 0) return 0;

    struct pool_page *p = (struct pool_page *)page;
    uint32_t size = bin_size[bin];

    p->chunk_size  = size;
    p->bin         = bin;
    p->used        = 0;
    p->total       = (uint32_t)((PAGE_SIZE - HEADER_SIZE) / size);
    p->free_chunks = 0;

    /* Threaded back to front so the list comes out in address order. */
    for (uint32_t i = p->total; i > 0; i--) {
        void *chunk = (uint8_t *)page + HEADER_SIZE + (uint64_t)(i - 1) * size;
        *(void **)chunk = p->free_chunks;
        p->free_chunks = chunk;
    }

    link_page(p);
    stat_pool_pages++;

    if (log_enabled) {
        uart_puts("  pool: new page ");
        uart_hex((uint64_t)(uintptr_t)page);
        uart_puts(" for ");
        uart_dec(size);
        uart_puts("-byte chunks (");
        uart_dec(p->total);
        uart_puts(" of them)\n");
    }

    return p;
}

void *kmalloc(size_t size) {
    if (size == 0) return 0;

    int bin = size_to_bin(size);

    if (bin < 0) {
        /* Too big to be worth chunking: give it whole frames. */
        int order = size_to_order(size);
        if (order < 0) return 0;

        void *p = page_alloc(order);
        if (p) stat_big_allocs++;

        if (log_enabled) {
            uart_puts("kmalloc(");
            uart_dec(size);
            uart_puts(") -> ");
            uart_hex((uint64_t)(uintptr_t)p);
            uart_puts("  order ");
            uart_dec(order);
            uart_puts(" page block\n");
        }
        return p;
    }

    uint64_t daif = irq_disable_save();

    struct pool_page *p = partial[bin];
    if (p == 0) {
        p = new_pool_page(bin);
        if (p == 0) { irq_restore(daif); return 0; }
    }

    void *chunk = p->free_chunks;
    p->free_chunks = *(void **)chunk;
    p->used++;
    if (p->free_chunks == 0) unlink_page(p);   /* nothing left to give */

    stat_chunk_allocs++;
    irq_restore(daif);

    if (log_enabled) {
        uart_puts("kmalloc(");
        uart_dec(size);
        uart_puts(") -> ");
        uart_hex((uint64_t)(uintptr_t)chunk);
        uart_puts("  ");
        uart_dec(bin_size[bin]);
        uart_puts("-byte bin\n");
    }

    return chunk;
}

void kfree(void *ptr) {
    if (ptr == 0) return;

    /* A pool chunk always sits past the page header, so a page-aligned pointer
     * can only have come from page_alloc. */
    if (((uint64_t)(uintptr_t)ptr & (PAGE_SIZE - 1)) == 0) {
        if (log_enabled) {
            uart_puts("kfree(");
            uart_hex((uint64_t)(uintptr_t)ptr);
            uart_puts(")  page block\n");
        }
        page_free(ptr);
        return;
    }

    uint64_t daif = irq_disable_save();

    struct pool_page *p =
        (struct pool_page *)((uint64_t)(uintptr_t)ptr & ~(PAGE_SIZE - 1));

    if (p->used == 0 || p->chunk_size == 0 || p->bin < 0 || p->bin >= NBINS) {
        irq_restore(daif);
        uart_puts("kfree: ");
        uart_hex((uint64_t)(uintptr_t)ptr);
        uart_puts(" does not belong to a live pool\n");
        return;
    }

    int was_full = (p->free_chunks == 0);
    *(void **)ptr = p->free_chunks;
    p->free_chunks = ptr;
    p->used--;
    stat_chunk_frees++;

    if (log_enabled) {
        uart_puts("kfree(");
        uart_hex((uint64_t)(uintptr_t)ptr);
        uart_puts(")  ");
        uart_dec(p->chunk_size);
        uart_puts("-byte bin, ");
        uart_dec(p->used);
        uart_puts(" still in use\n");
    }

    if (was_full) link_page(p);

    if (p->used == 0) {
        /* Every chunk is back: hand the frame to the buddy system, where it
         * can merge with its neighbours again. */
        unlink_page(p);
        p->chunk_size = 0;
        stat_pool_pages--;
        if (log_enabled) {
            uart_puts("  pool: page ");
            uart_hex((uint64_t)(uintptr_t)p);
            uart_puts(" is empty, returning it to the buddy system\n");
        }
        page_free(p);
    }

    irq_restore(daif);
}

void kmalloc_report(void) {
    uart_puts("chunk allocs: ");
    uart_dec(stat_chunk_allocs);
    uart_puts("   chunk frees: ");
    uart_dec(stat_chunk_frees);
    uart_puts("   page-sized allocs: ");
    uart_dec(stat_big_allocs);
    uart_puts("\npool pages in use: ");
    uart_dec(stat_pool_pages);
    uart_puts("\nbins:\n");

    uint64_t daif = irq_disable_save();
    for (int i = 0; i < NBINS; i++) {
        uint64_t pages = 0, freechunks = 0;
        for (struct pool_page *p = partial[i]; p; p = p->next) {
            pages++;
            freechunks += (p->total - p->used);
        }
        if (pages == 0) continue;

        uart_puts("  ");
        uart_dec(bin_size[i]);
        uart_puts(" bytes:\t");
        uart_dec(pages);
        uart_puts(" partial page(s), ");
        uart_dec(freechunks);
        uart_puts(" chunk(s) free\n");
    }
    irq_restore(daif);
}

#include "bcache.h"
#include "sd.h"
#include "page.h"
#include "mm.h"
#include "uart.h"
#include "string.h"

#define BLOCKS_PER_PAGE (PAGE_SIZE / SD_BLOCK_SIZE)     /* 8 */

struct cache_page {
    uint32_t base;              /* block number of the first block in the page */
    uint8_t *data;              /* one frame */
    uint8_t  dirty;             /* one bit per block */
    struct cache_page *next;
};

static struct cache_page *pages;
static uint64_t stat_hits, stat_misses, stat_pages;

static struct cache_page *find_page(uint32_t base) {
    for (struct cache_page *p = pages; p; p = p->next) {
        if (p->base == base) return p;
    }
    return 0;
}

/* Fills a fresh frame from the card. A block that will not read is left zero
 * rather than failing the whole page: the caller may only want one of the
 * eight, and the page may run past the end of the medium. */
static struct cache_page *load_page(uint32_t base) {
    struct cache_page *p = kmalloc(sizeof(struct cache_page));
    if (p == 0) return 0;

    p->data = page_alloc(0);
    if (p->data == 0) {
        kfree(p);
        return 0;
    }

    p->base  = base;
    p->dirty = 0;

    for (uint32_t i = 0; i < BLOCKS_PER_PAGE; i++) {
        if (readblock(base + i, p->data + i * SD_BLOCK_SIZE) != 0)
            memset(p->data + i * SD_BLOCK_SIZE, 0, SD_BLOCK_SIZE);
    }

    p->next = pages;
    pages   = p;
    stat_pages++;
    stat_misses++;
    return p;
}

uint8_t *bcache_get(uint32_t lba) {
    if (!sd_present()) return 0;

    uint32_t base = lba & ~(BLOCKS_PER_PAGE - 1);
    struct cache_page *p = find_page(base);

    if (p) stat_hits++;
    else   p = load_page(base);

    if (p == 0) return 0;
    return p->data + (lba - base) * SD_BLOCK_SIZE;
}

void bcache_mark_dirty(uint32_t lba) {
    uint32_t base = lba & ~(BLOCKS_PER_PAGE - 1);
    struct cache_page *p = find_page(base);

    if (p) p->dirty |= (uint8_t)(1u << (lba - base));
}

int bcache_sync(void) {
    int written = 0;

    for (struct cache_page *p = pages; p; p = p->next) {
        if (p->dirty == 0) continue;

        for (uint32_t i = 0; i < BLOCKS_PER_PAGE; i++) {
            if (!(p->dirty & (1u << i))) continue;

            if (writeblock(p->base + i, p->data + i * SD_BLOCK_SIZE) != 0) {
                uart_puts("sync: a block would not write back\n");
                continue;
            }
            written++;
        }

        p->dirty = 0;
    }

    return written;
}

void bcache_stats(uint64_t *hits, uint64_t *misses, uint64_t *page_count,
                  uint64_t *dirty_blocks) {
    if (hits)       *hits       = stat_hits;
    if (misses)     *misses     = stat_misses;
    if (page_count) *page_count = stat_pages;

    if (dirty_blocks) {
        uint64_t n = 0;
        for (struct cache_page *p = pages; p; p = p->next) {
            for (uint32_t i = 0; i < BLOCKS_PER_PAGE; i++) {
                if (p->dirty & (1u << i)) n++;
            }
        }
        *dirty_blocks = n;
    }
}

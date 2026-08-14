#include "fdt.h"
#include "uart.h"
#include "string.h"
#include "cpio.h"

/* Flattened devicetree, as specified in the Devicetree Specification ch. 5.
 * Every integer in the blob is big-endian, so nothing may be dereferenced as a
 * native uint32_t -- it all goes through fdt_be32(). */
struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

#define FDT_MAGIC        0xD00DFEEDu
#define FDT_BEGIN_NODE   0x00000001u
#define FDT_END_NODE     0x00000002u
#define FDT_PROP         0x00000003u
#define FDT_NOP          0x00000004u
#define FDT_END          0x00000009u

/* Deep enough for any real board tree; guards the node-name stack. */
#define FDT_MAX_DEPTH    32

/* QEMU's raspi3b puts an initrd here when -initrd is given without a
 * devicetree that says otherwise; the Rpi3 config.txt line in the lab uses
 * 0x20000000. Only used when the devicetree does not tell us. */
#define INITRAMFS_FALLBACK 0x8000000UL

static uint64_t fdt_base;

uint32_t fdt_be32(const void *p) {
    const uint8_t *b = (const uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}

static uint64_t fdt_be64(const void *p) {
    return ((uint64_t)fdt_be32(p) << 32) | fdt_be32((const uint8_t *)p + 4);
}

static uint32_t align4(uint32_t x) { return (x + 3u) & ~3u; }

/* Validates the header before anything trusts an offset out of it. Returns the
 * header or NULL -- the kernel is booted with whatever the firmware left in
 * x0, which on QEMU without -dtb is an ATAGS pointer, not a devicetree. */
static const struct fdt_header *fdt_check(uint64_t addr) {
    if (addr == 0 || (addr & 3) != 0) return 0;

    const struct fdt_header *h = (const struct fdt_header *)addr;
    if (fdt_be32(&h->magic) != FDT_MAGIC) return 0;

    uint32_t total  = fdt_be32(&h->totalsize);
    uint32_t soff   = fdt_be32(&h->off_dt_struct);
    uint32_t ssize  = fdt_be32(&h->size_dt_struct);
    uint32_t stroff = fdt_be32(&h->off_dt_strings);
    uint32_t strsz  = fdt_be32(&h->size_dt_strings);

    /* Every block must lie inside the blob, or a later offset walks off the
     * end of it. Checked as sums that cannot overflow at these magnitudes. */
    if (total < sizeof(struct fdt_header) || total > 0x400000u) return 0;
    if (soff > total || ssize > total - soff) return 0;
    if (stroff > total || strsz > total - stroff) return 0;

    return h;
}

int fdt_traverse(fdt_callback cb, void *arg) {
    const struct fdt_header *h = fdt_check(fdt_base);
    if (h == 0 || cb == 0) return -1;

    const uint8_t *blob    = (const uint8_t *)h;
    const uint8_t *strings = blob + fdt_be32(&h->off_dt_strings);
    uint32_t       strsize = fdt_be32(&h->size_dt_strings);
    const uint8_t *p       = blob + fdt_be32(&h->off_dt_struct);
    const uint8_t *end     = p + fdt_be32(&h->size_dt_struct);

    /* Name of the node each property belongs to, one slot per nesting level. */
    const char *stack[FDT_MAX_DEPTH];
    int depth = 0;

    while (p + 4 <= end) {
        uint32_t token = fdt_be32(p);
        p += 4;

        if (token == FDT_BEGIN_NODE) {
            const char *name = (const char *)p;
            uint32_t len = 0;

            while (p + len < end && p[len] != '\0') len++;
            if (p + len >= end) return -1;          /* unterminated node name */

            if (depth < FDT_MAX_DEPTH) stack[depth] = name;
            depth++;

            p += align4(len + 1);

        } else if (token == FDT_END_NODE) {
            if (depth == 0) return -1;              /* unbalanced tree */
            depth--;

        } else if (token == FDT_PROP) {
            if (p + 8 > end) return -1;

            uint32_t len     = fdt_be32(p);
            uint32_t nameoff = fdt_be32(p + 4);
            p += 8;

            if (nameoff >= strsize) return -1;
            if (len > (uint32_t)(end - p)) return -1;

            const char *node = (depth > 0 && depth <= FDT_MAX_DEPTH)
                             ? stack[depth - 1] : "";
            cb(node, (const char *)(strings + nameoff), p, len, arg);

            p += align4(len);

        } else if (token == FDT_NOP) {
            /* nothing to do */

        } else if (token == FDT_END) {
            return 0;

        } else {
            return -1;                              /* unknown token */
        }
    }

    return 0;
}

uint64_t fdt_get_base(void) { return fdt_base; }

/* Picks the initramfs address out of /chosen. The property is written by
 * whoever loaded the ramdisk (QEMU's -initrd, or the Rpi firmware from the
 * initramfs line in config.txt) and is a u32 or u64 depending on the writer. */
static void initramfs_callback(const char *node, const char *prop,
                               const void *val, uint32_t len, void *arg) {
    uint64_t *found = (uint64_t *)arg;

    if (strcmp(node, "chosen") != 0) return;
    if (strcmp(prop, "linux,initrd-start") != 0) return;

    if (len == 4)      *found = fdt_be32(val);
    else if (len == 8) *found = fdt_be64(val);
}

uint64_t fdt_get_totalsize(void) {
    const struct fdt_header *h = fdt_check(fdt_base);
    return h ? fdt_be32(&h->totalsize) : 0;
}

/* A memory node is named exactly "memory" or "memory@<address>"; matching on
 * the prefix alone would also pick up unrelated nodes such as
 * "memory-controller". */
int fdt_is_memory_node(const char *node) {
    if (strncmp(node, "memory", 6) != 0) return 0;
    return node[6] == '\0' || node[6] == '@';
}

/* Reads one cell pair out of a reg property. The Rpi3 root declares one
 * address cell and one size cell, so each pair is eight bytes. */
struct memory_query { uint64_t base, size; int found; };

static void memory_query_cb(const char *node, const char *prop,
                            const void *val, uint32_t len, void *arg) {
    struct memory_query *q = (struct memory_query *)arg;

    if (q->found) return;
    if (!fdt_is_memory_node(node)) return;
    if (strcmp(prop, "reg") != 0 || len < 8) return;

    q->base  = fdt_be32(val);
    q->size  = fdt_be32((const uint8_t *)val + 4);
    q->found = 1;
}

int fdt_get_memory(uint64_t *base, uint64_t *size) {
    struct memory_query q = { 0, 0, 0 };

    if (fdt_traverse(memory_query_cb, &q) != 0 || !q.found) return -1;

    /* The size is filled in by the firmware at boot: the blob shipped on disk
     * carries zero. Treating that as the answer would leave the page allocator
     * with no frames at all, so fall back instead. */
    if (q.size == 0) return -1;

    if (base) *base = q.base;
    if (size) *size = q.size;
    return 0;
}

struct initrd_query { uint64_t start, end; int have_start, have_end; };

static void initrd_query_cb(const char *node, const char *prop,
                            const void *val, uint32_t len, void *arg) {
    struct initrd_query *q = (struct initrd_query *)arg;
    uint64_t v;

    /* These live in /chosen; accepting them from anywhere would let an
     * unrelated node redirect the initramfs. */
    if (strcmp(node, "chosen") != 0) return;

    if (len == 4)      v = fdt_be32(val);
    else if (len == 8) v = fdt_be64(val);
    else return;

    if (strcmp(prop, "linux,initrd-start") == 0) { q->start = v; q->have_start = 1; }
    else if (strcmp(prop, "linux,initrd-end") == 0) { q->end = v; q->have_end = 1; }
}

int fdt_get_initrd(uint64_t *start, uint64_t *end) {
    struct initrd_query q = { 0, 0, 0, 0 };

    if (fdt_traverse(initrd_query_cb, &q) != 0) return -1;
    if (!q.have_start || !q.have_end) return -1;

    if (start) *start = q.start;
    if (end)   *end   = q.end;
    return 0;
}

void fdt_init(uint64_t dtb_addr) {
    fdt_base = fdt_check(dtb_addr) ? dtb_addr : 0;

    uint64_t initramfs = 0;
    if (fdt_base) fdt_traverse(initramfs_callback, &initramfs);

    if (initramfs == 0) initramfs = INITRAMFS_FALLBACK;
    cpio_set_base((void *)initramfs);
}

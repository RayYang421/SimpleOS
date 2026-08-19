#include "vfs.h"
#include "bcache.h"
#include "sd.h"
#include "mm.h"
#include "uart.h"
#include "string.h"

/* FAT32 on the SD card.
 *
 * Every access goes through the page cache rather than the driver, so reads
 * are served from memory once the block has been touched and writes stay in
 * memory until sync asks for them. Nothing in here calls readblock or
 * writeblock directly.
 *
 * Names are the original 8.3 short form: eleven bytes, stem padded to eight
 * and extension to three, upper case. */

#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0F

#define FAT_FREE       0x00000000u
#define FAT_EOC        0x0FFFFFF8u      /* and anything above it */
#define FAT_MASK       0x0FFFFFFFu
#define DIRENT_SIZE    32

/* A directory entry records a length in 32 bits, so this is the format's own
 * ceiling rather than a policy of ours -- and the read and write loops narrow
 * the file position to the same width, which past here would wrap and start
 * again over the beginning of the file. */
#define FAT_MAX_FILE_SIZE 0xFFFFFFFFUL

#define FAT_FILE 0
#define FAT_DIR  1

static struct {
    uint32_t part_lba;
    uint32_t fat_start;
    uint32_t fat_sectors;
    uint32_t data_start;
    uint32_t root_cluster;
    uint32_t sectors_per_cluster;
    uint32_t cluster_bytes;
    uint32_t num_fats;
    uint32_t clusters;
    int mounted;
} fat;

struct fat_node {
    char name[MAX_COMPONENT_LEN + 1];
    int  type;

    struct vnode *parent;

    uint32_t first_cluster;
    uint32_t size;

    /* Where this node's own directory entry sits, so a new size or a new
     * starting cluster can be put back. Zero for the root, which has none. */
    uint32_t dirent_lba;
    uint32_t dirent_off;

    /* Children, built the first time the directory is looked in: the lab's
     * "component name cache". */
    struct vnode *entries[MAX_DIR_ENTRIES];
    int nentries;
    int scanned;
};

static struct vnode_operations fat_v_ops;
static struct file_operations  fat_f_ops;

static struct fat_node *node_of(struct vnode *v) {
    return (struct fat_node *)v->internal;
}

/* --- little-endian fields ---------------------------------------------------
 *
 * Assembled a byte at a time rather than cast: a directory entry is only
 * two-byte aligned within a sector, and the fields do not care what the CPU's
 * byte order happens to be. */

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* --- geometry ---------------------------------------------------------------
 *
 * Everything below reads cluster numbers off the card, so none of them can be
 * trusted: a number out of range, or one that overflows the arithmetic that
 * turns it into a sector, would otherwise send a write somewhere it has no
 * business being -- the allocation table, or the partition table. */

static void fat_corrupt(const char *what) {
    uart_puts("fat32: ");
    uart_puts(what);
    uart_puts("\n");
}

/* A cluster this volume actually has. The upper bound is the number of
 * clusters in the data area, not the format's reserved range: everything from
 * there to FAT_EOC is a number the card can hold but the volume cannot use. */
static int cluster_valid(uint32_t cluster) {
    return cluster >= 2 && cluster < fat.clusters;
}

/* The first sector of a cluster, or 0 if that lands outside a 32-bit sector
 * number. Worked out in 64 bits so the overflow is visible rather than being
 * the thing that produces the answer; a real volume's data never starts at
 * sector 0, so 0 is free to mean "no such sector". */
static uint32_t cluster_sector(uint32_t cluster) {
    uint64_t sector = (uint64_t)fat.data_start +
                      (uint64_t)(cluster - 2) * fat.sectors_per_cluster;

    if (sector > 0xFFFFFFFFUL) {
        fat_corrupt("a cluster number puts its sector past the end of the card");
        return 0;
    }

    return (uint32_t)sector;
}

/* The block of the allocation table holding this cluster's entry, or 0 if the
 * entry would fall outside the table. */
static uint32_t fat_entry_block(uint32_t cluster, uint32_t copy,
                                uint32_t *offset_in_block) {
    uint64_t off = (uint64_t)cluster * 4;
    uint64_t block = off / SD_BLOCK_SIZE;

    if (block >= fat.fat_sectors) return 0;

    *offset_in_block = (uint32_t)(off % SD_BLOCK_SIZE);
    return fat.fat_start + copy * fat.fat_sectors + (uint32_t)block;
}

static uint32_t fat_get(uint32_t cluster) {
    uint32_t off;
    uint32_t lba = fat_entry_block(cluster, 0, &off);

    /* Outside the table: the entry does not exist, and the safe answer is the
     * one that ends a chain rather than continues it somewhere else. */
    if (lba == 0) return FAT_EOC;

    uint8_t *sector = bcache_get(lba);
    if (sector == 0) return FAT_EOC;

    return le32(sector + off) & FAT_MASK;
}

/* Written to every copy of the table: a host that checks the image compares
 * them, and one stale copy makes it call the filesystem corrupt. */
static void fat_set(uint32_t cluster, uint32_t value) {
    for (uint32_t copy = 0; copy < fat.num_fats; copy++) {
        uint32_t off;
        uint32_t lba = fat_entry_block(cluster, copy, &off);

        if (lba == 0) {
            fat_corrupt("refusing to write a table entry outside the table");
            return;
        }

        uint8_t *sector = bcache_get(lba);
        if (sector == 0) return;

        put32(sector + off, value & FAT_MASK);
        bcache_mark_dirty(lba);
    }
}

/* --- following a chain -------------------------------------------------------
 *
 * A chain that leads back into itself would be followed for ever. Two bounds
 * stop that. The step counter is the plain one: a chain cannot be longer than
 * the volume has clusters, so a walk that takes more steps than that is
 * corrupt whatever it looks like. The second cursor, stepping half as often,
 * lands on the first within one lap of any loop, which catches the same fault
 * in the length of the loop rather than the length of the volume -- on a large
 * card the difference is between failing at once and appearing to hang, which
 * is the very thing being guarded against. */
struct chain {
    uint32_t trailing;
    uint32_t steps;
    int      half_step;
};

static void chain_start(struct chain *c, uint32_t first) {
    c->trailing  = first;
    c->steps     = 0;
    c->half_step = 0;
}

/* Moves *cluster on by one. Returns -1 when the chain has been caught leading
 * back into itself. */
static int chain_advance(struct chain *c, uint32_t *cluster) {
    if (++c->steps >= fat.clusters) {
        fat_corrupt("a cluster chain is longer than the volume");
        return -1;
    }

    *cluster = fat_get(*cluster);

    if (c->half_step) c->trailing = fat_get(c->trailing);
    c->half_step = !c->half_step;

    if (cluster_valid(*cluster) && *cluster == c->trailing) {
        fat_corrupt("a cluster chain leads back into itself");
        return -1;
    }

    return 0;
}

static uint32_t alloc_cluster(void) {
    for (uint32_t cluster = 2; cluster < fat.clusters; cluster++) {
        if (fat_get(cluster) == FAT_FREE) {
            fat_set(cluster, FAT_MASK);         /* end of chain */
            return cluster;
        }
    }
    return 0;
}

/* --- names ------------------------------------------------------------------ */

static char upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* "fat_r.txt" -> "FAT_R   TXT" */
static void name_to_83(const char *name, char *out) {
    int i = 0;

    for (int n = 0; n < 11; n++) out[n] = ' ';

    while (name[i] && name[i] != '.' && i < 8) { out[i] = upper(name[i]); i++; }
    while (name[i] && name[i] != '.') i++;

    if (name[i] != '.') return;
    i++;

    for (int n = 0; n < 3 && name[i]; n++, i++) out[8 + n] = upper(name[i]);
}

/* "FAT_R   TXT" -> "FAT_R.TXT" */
static void name_from_83(const uint8_t *raw, char *out) {
    int n = 0;

    for (int i = 0; i < 8 && raw[i] != ' '; i++) out[n++] = (char)raw[i];

    if (raw[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) out[n++] = (char)raw[i];
    }

    out[n] = '\0';
}

static int name_equal(const char *a, const char *b) {
    int i = 0;
    while (a[i] && b[i]) {
        if (upper(a[i]) != upper(b[i])) return 0;
        i++;
    }
    return a[i] == b[i];
}

/* --- nodes ------------------------------------------------------------------ */

static struct vnode *new_node(const char *name, int type, struct vnode *parent,
                              uint32_t cluster, uint32_t size,
                              uint32_t dirent_lba, uint32_t dirent_off) {
    struct vnode    *v = kmalloc(sizeof(struct vnode));
    struct fat_node *n = kmalloc(sizeof(struct fat_node));

    if (v == 0 || n == 0) {
        if (v) kfree(v);
        if (n) kfree(n);
        return 0;
    }

    memset(n, 0, sizeof(*n));

    int i = 0;
    while (name[i] && i < MAX_COMPONENT_LEN) { n->name[i] = name[i]; i++; }
    n->name[i] = '\0';

    n->type          = type;
    n->parent        = parent ? parent : v;
    n->first_cluster = cluster;
    n->size          = size;
    n->dirent_lba    = dirent_lba;
    n->dirent_off    = dirent_off;

    v->mount    = 0;
    v->v_ops    = &fat_v_ops;
    v->f_ops    = (type == FAT_DIR) ? 0 : &fat_f_ops;
    v->internal = n;

    return v;
}

/* Puts a node's size and starting cluster back into its directory entry. The
 * entry only reaches the card when sync does. */
static void update_dirent(struct fat_node *n) {
    if (n->dirent_lba == 0) return;

    uint8_t *sector = bcache_get(n->dirent_lba);
    if (sector == 0) return;

    uint8_t *e = sector + n->dirent_off;
    put16(e + 20, (uint16_t)(n->first_cluster >> 16));
    put16(e + 26, (uint16_t)(n->first_cluster & 0xFFFF));
    put32(e + 28, n->size);

    bcache_mark_dirty(n->dirent_lba);
}

/* Reads a directory's entries once and keeps the vnodes. */
static void scan_directory(struct vnode *dir) {
    struct fat_node *d = node_of(dir);

    if (d->scanned) return;
    d->scanned = 1;

    uint32_t cluster = d->first_cluster;
    struct chain walk;

    chain_start(&walk, cluster);

    while (cluster_valid(cluster)) {
        uint32_t base = cluster_sector(cluster);
        if (base == 0) return;

        for (uint32_t s = 0; s < fat.sectors_per_cluster; s++) {
            uint32_t lba = base + s;
            uint8_t *sector = bcache_get(lba);
            if (sector == 0) return;

            for (uint32_t off = 0; off < SD_BLOCK_SIZE; off += DIRENT_SIZE) {
                uint8_t *e = sector + off;

                if (e[0] == 0x00) return;               /* no entries past here */
                if (e[0] == 0xE5) continue;             /* deleted */
                if (e[11] == ATTR_LFN) continue;        /* long-name fragment */
                if (e[11] & ATTR_VOLUME_ID) continue;   /* the volume label */

                if (d->nentries >= MAX_DIR_ENTRIES) return;

                char name[MAX_COMPONENT_LEN + 1];
                name_from_83(e, name);

                uint32_t cl = ((uint32_t)le16(e + 20) << 16) | le16(e + 26);
                struct vnode *child =
                    new_node(name, (e[11] & ATTR_DIRECTORY) ? FAT_DIR : FAT_FILE,
                             dir, cl, le32(e + 28), lba, off);

                if (child == 0) return;
                d->entries[d->nentries++] = child;
            }
        }

        if (chain_advance(&walk, &cluster) != 0) return;
    }
}

static int fat_lookup(struct vnode *dir, struct vnode **target,
                      const char *name) {
    struct fat_node *d = node_of(dir);

    if (d->type != FAT_DIR) return -1;

    if (strcmp(name, ".") == 0)  { *target = dir;       return 0; }
    if (strcmp(name, "..") == 0) { *target = d->parent; return 0; }

    scan_directory(dir);

    for (int i = 0; i < d->nentries; i++) {
        if (name_equal(node_of(d->entries[i])->name, name)) {
            *target = d->entries[i];
            return 0;
        }
    }

    return -1;
}

static int fat_readdir(struct vnode *dir, int index, char *name) {
    struct fat_node *d = node_of(dir);

    if (d->type != FAT_DIR) return -1;
    scan_directory(dir);
    if (index < 0 || index >= d->nentries) return -1;

    const char *src = node_of(d->entries[index])->name;
    int i = 0;
    while (src[i]) { name[i] = src[i]; i++; }
    name[i] = '\0';
    return 0;
}

/* Finds a directory entry that is free or deleted, extending the directory by
 * a cluster if every one is in use. */
static int find_free_dirent(struct vnode *dir, uint32_t *out_lba,
                            uint32_t *out_off) {
    struct fat_node *d = node_of(dir);
    uint32_t cluster = d->first_cluster;
    uint32_t last = cluster;
    struct chain walk;

    chain_start(&walk, cluster);

    while (cluster_valid(cluster)) {
        uint32_t base = cluster_sector(cluster);
        if (base == 0) return -1;

        for (uint32_t s = 0; s < fat.sectors_per_cluster; s++) {
            uint32_t lba = base + s;
            uint8_t *sector = bcache_get(lba);
            if (sector == 0) return -1;

            for (uint32_t off = 0; off < SD_BLOCK_SIZE; off += DIRENT_SIZE) {
                if (sector[off] == 0x00 || sector[off] == 0xE5) {
                    *out_lba = lba;
                    *out_off = off;
                    return 0;
                }
            }
        }

        last = cluster;
        if (chain_advance(&walk, &cluster) != 0) return -1;
    }

    /* Every entry is taken: give the directory another cluster and use its
     * first entry. */
    uint32_t fresh = alloc_cluster();
    if (fresh == 0) return -1;

    uint32_t base = cluster_sector(fresh);
    if (base == 0) return -1;

    fat_set(last, fresh);

    for (uint32_t s = 0; s < fat.sectors_per_cluster; s++) {
        uint32_t lba = base + s;
        uint8_t *sector = bcache_get(lba);
        if (sector == 0) return -1;

        memset(sector, 0, SD_BLOCK_SIZE);
        bcache_mark_dirty(lba);
    }

    *out_lba = base;
    *out_off = 0;
    return 0;
}

static int fat_create(struct vnode *dir, struct vnode **target,
                      const char *name) {
    struct fat_node *d = node_of(dir);
    struct vnode *existing;

    if (!fat.mounted || d->type != FAT_DIR) return -1;
    if (fat_lookup(dir, &existing, name) == 0) return -1;
    if (d->nentries >= MAX_DIR_ENTRIES) return -1;

    uint32_t lba, off;
    if (find_free_dirent(dir, &lba, &off) != 0) return -1;

    uint32_t cluster = alloc_cluster();
    if (cluster == 0) return -1;

    uint8_t *sector = bcache_get(lba);
    if (sector == 0) return -1;

    uint8_t *e = sector + off;
    memset(e, 0, DIRENT_SIZE);
    name_to_83(name, (char *)e);
    e[11] = ATTR_ARCHIVE;
    put16(e + 20, (uint16_t)(cluster >> 16));
    put16(e + 26, (uint16_t)(cluster & 0xFFFF));
    put32(e + 28, 0);
    bcache_mark_dirty(lba);

    /* Nothing marks the end of a directory but a zeroed entry, and the entry
     * after this one is already zero: a cluster is zeroed when the directory
     * is extended, and mkfs zeroes the first one. */

    char shown[MAX_COMPONENT_LEN + 1];
    name_from_83(e, shown);

    struct vnode *v = new_node(shown, FAT_FILE, dir, cluster, 0, lba, off);
    if (v == 0) return -1;

    d->entries[d->nentries++] = v;
    *target = v;
    return 0;
}

/* FAT directories need "." and ".." entries of their own, which nothing here
 * would create. Reading and writing files is what the lab asks for. */
static int fat_mkdir(struct vnode *dir, struct vnode **target,
                     const char *name) {
    (void)dir; (void)target; (void)name;
    return -1;
}

/* --- file contents ---------------------------------------------------------- */

/* The cluster holding a given offset, allocating and linking one when the
 * chain is too short and the caller is writing. */
static uint32_t cluster_at(struct fat_node *n, uint32_t pos, int extend) {
    uint32_t cluster = n->first_cluster;
    uint32_t skip = pos / fat.cluster_bytes;
    struct chain walk;

    if (!cluster_valid(cluster)) {
        /* Zero is what a file with nothing in it looks like. Any other number
         * that fails the check is one the volume cannot have, and allocating a
         * replacement would quietly rewrite a corrupt entry as though it had
         * always been empty. */
        if (cluster != 0) {
            fat_corrupt("a file starts at a cluster the volume does not have");
            return 0;
        }

        if (!extend) return 0;

        cluster = alloc_cluster();
        if (cluster == 0) return 0;

        n->first_cluster = cluster;
        update_dirent(n);
    }

    chain_start(&walk, cluster);

    while (skip--) {
        uint32_t next = cluster;
        if (chain_advance(&walk, &next) != 0) return 0;

        if (!cluster_valid(next)) {
            /* Past the end of the range is the marker for the end of a chain,
             * and extending from there is ordinary. Anything else is a number
             * that has no business being in the table. */
            if (next < FAT_EOC) {
                fat_corrupt("a cluster chain leads outside the volume");
                return 0;
            }

            if (!extend) return 0;

            next = alloc_cluster();
            if (next == 0) return 0;
            fat_set(cluster, next);
        }

        cluster = next;
    }

    return cluster;
}

static int fat_read(struct file *file, void *buf, size_t len) {
    struct fat_node *n = node_of(file->vnode);
    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;

    if (n->type != FAT_FILE) return -1;
    if (file->f_pos >= n->size) return 0;

    /* By subtraction: adding a user-supplied len to f_pos can wrap. */
    size_t room = n->size - file->f_pos;
    if (len > room) len = room;

    while (done < len) {
        uint32_t pos     = (uint32_t)file->f_pos;
        uint32_t cluster = cluster_at(n, pos, 0);
        if (!cluster_valid(cluster)) break;

        uint32_t base = cluster_sector(cluster);
        if (base == 0) break;

        uint32_t in_cluster = pos % fat.cluster_bytes;
        uint32_t lba = base + in_cluster / SD_BLOCK_SIZE;
        uint32_t in_sector = in_cluster % SD_BLOCK_SIZE;

        uint8_t *sector = bcache_get(lba);
        if (sector == 0) break;

        size_t chunk = SD_BLOCK_SIZE - in_sector;
        if (chunk > len - done) chunk = len - done;

        memcpy(out + done, sector + in_sector, chunk);
        done += chunk;
        file->f_pos += chunk;
    }

    return (int)done;
}

static int fat_write(struct file *file, const void *buf, size_t len) {
    struct fat_node *n = node_of(file->vnode);
    const uint8_t *in = (const uint8_t *)buf;
    size_t done = 0;

    if (!fat.mounted || n->type != FAT_FILE) return -1;
    if (file->f_pos >= FAT_MAX_FILE_SIZE) return -1;

    /* Bounded by subtraction, and bounded at all: without this the loop below
     * would keep extending the cluster chain for as long as a user-supplied
     * length asked it to. */
    size_t room = FAT_MAX_FILE_SIZE - file->f_pos;
    if (len > room) len = room;

    while (done < len) {
        uint32_t pos     = (uint32_t)file->f_pos;
        uint32_t cluster = cluster_at(n, pos, 1);
        if (!cluster_valid(cluster)) break;

        uint32_t base = cluster_sector(cluster);
        if (base == 0) break;

        uint32_t in_cluster = pos % fat.cluster_bytes;
        uint32_t lba = base + in_cluster / SD_BLOCK_SIZE;
        uint32_t in_sector = in_cluster % SD_BLOCK_SIZE;

        uint8_t *sector = bcache_get(lba);
        if (sector == 0) break;

        size_t chunk = SD_BLOCK_SIZE - in_sector;
        if (chunk > len - done) chunk = len - done;

        memcpy(sector + in_sector, in + done, chunk);
        bcache_mark_dirty(lba);

        done += chunk;
        file->f_pos += chunk;
    }

    if (file->f_pos > n->size) {
        n->size = (uint32_t)file->f_pos;
        update_dirent(n);
    }

    return (int)done;
}

static int fat_open(struct vnode *node, struct file **target) {
    (void)node; (void)target;
    return 0;
}

static int fat_close(struct file *file) {
    (void)file;
    return 0;
}

/* The same ceiling as fat_write, and for the same reason: a position past it
 * cannot be recorded, and seeking there and writing one byte would ask for a
 * cluster chain reaching all the way out to it. */
static long fat_lseek64(struct file *file, long offset, int whence) {
    if (whence != SEEK_SET || offset < 0) return -1;
    if ((uint64_t)offset > FAT_MAX_FILE_SIZE) return -1;

    file->f_pos = (size_t)offset;
    return offset;
}

/* --- mounting --------------------------------------------------------------- */

/* Finds the first FAT32 partition in the master boot record. */
static int find_partition(uint32_t *lba) {
    uint8_t *mbr = bcache_get(0);
    if (mbr == 0) return -1;

    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return -1;

    for (int i = 0; i < 4; i++) {
        uint8_t *e = mbr + 0x1BE + i * 16;

        /* 0x0B is FAT32 with CHS addressing, 0x0C the same with LBA. */
        if (e[4] != 0x0B && e[4] != 0x0C) continue;

        *lba = le32(e + 8);
        return 0;
    }

    fat_corrupt("no FAT32 partition in the table");
    return -1;
}

/* Every number below decides where a later read or write lands, so each one is
 * checked here rather than trusted and used. A volume that fails any of them
 * is refused outright: carrying on with a substituted value would mean writing
 * to wherever the substitution happened to point. */
static int read_boot_sector(uint32_t part_lba) {
    uint8_t *b = bcache_get(part_lba);
    if (b == 0) return -1;

    if (b[510] != 0x55 || b[511] != 0xAA) {
        fat_corrupt("the boot sector has no signature");
        return -1;
    }

    uint32_t bytes_per_sector   = le16(b + 0x0B);
    uint32_t sectors_per_cluster = b[0x0D];
    uint32_t reserved           = le16(b + 0x0E);
    uint32_t num_fats           = b[0x10];
    uint32_t fat_sectors        = le32(b + 0x24);
    uint32_t total_sectors      = le32(b + 0x20);
    uint32_t root_cluster       = le32(b + 0x2C);

    /* The driver moves 512 bytes at a time, so anything else would need the
     * block layer to gather several. */
    if (bytes_per_sector != SD_BLOCK_SIZE) {
        fat_corrupt("the sector size is not 512 bytes");
        return -1;
    }

    /* A power of two from 1 to 128, which is what the format allows and what
     * keeps a cluster from being larger than 64 KiB. */
    if (sectors_per_cluster == 0 || sectors_per_cluster > 128 ||
        (sectors_per_cluster & (sectors_per_cluster - 1)) != 0) {
        fat_corrupt("the cluster size is not a power of two");
        return -1;
    }

    /* With no copies of the table, every write recording an allocation would
     * go nowhere and the chain would never be written down. */
    if (num_fats == 0 || num_fats > 4) {
        fat_corrupt("the number of allocation tables is implausible");
        return -1;
    }

    if (reserved == 0 || fat_sectors == 0) {
        fat_corrupt("the reserved or table region is empty");
        return -1;
    }

    /* In 64 bits, because these come off the card and their sum in 32 would be
     * free to wrap back into the range that looks reasonable. */
    uint64_t metadata = (uint64_t)reserved + (uint64_t)num_fats * fat_sectors;

    if (metadata >= total_sectors) {
        fat_corrupt("the volume has no room for data after its metadata");
        return -1;
    }

    uint64_t data_sectors = (uint64_t)total_sectors - metadata;
    uint64_t clusters     = data_sectors / sectors_per_cluster + 2;

    uint64_t data_start = (uint64_t)part_lba + metadata;
    if (data_start > 0xFFFFFFFFUL || clusters > 0x0FFFFFF0UL) {
        fat_corrupt("the volume describes more space than the card can hold");
        return -1;
    }

    fat.part_lba            = part_lba;
    fat.sectors_per_cluster = sectors_per_cluster;
    fat.num_fats            = num_fats;
    fat.fat_sectors         = fat_sectors;
    fat.root_cluster        = root_cluster;
    fat.fat_start           = part_lba + reserved;
    fat.data_start          = (uint32_t)data_start;
    fat.cluster_bytes       = sectors_per_cluster * SD_BLOCK_SIZE;
    fat.clusters            = (uint32_t)clusters;

    /* Checked last, because cluster_valid needs the fields above. */
    if (!cluster_valid(fat.root_cluster)) {
        fat_corrupt("the root directory is outside the volume");
        return -1;
    }

    return 0;
}

static int fat_setup_mount(struct filesystem *fs, struct mount *mount) {
    (void)fs;

    /* Cleared first, so a mount that is refused cannot leave the previous
     * volume's geometry behind for something else to use. */
    fat.mounted = 0;

    if (!sd_present()) return -1;

    uint32_t part_lba;
    if (find_partition(&part_lba) != 0) return -1;
    if (read_boot_sector(part_lba) != 0) return -1;

    struct vnode *root = new_node("/", FAT_DIR, 0, fat.root_cluster, 0, 0, 0);
    if (root == 0) return -1;

    fat.mounted = 1;
    mount->root = root;
    return 0;
}

static struct vnode_operations fat_v_ops = {
    .lookup  = fat_lookup,
    .create  = fat_create,
    .mkdir   = fat_mkdir,
    .readdir = fat_readdir,
};

static struct file_operations fat_f_ops = {
    .write   = fat_write,
    .read    = fat_read,
    .open    = fat_open,
    .close   = fat_close,
    .lseek64 = fat_lseek64,
    .ioctl   = 0,
};

static struct filesystem fat32 = {
    .name        = "fat32",
    .setup_mount = fat_setup_mount,
};

void fat32_register(void) {
    register_filesystem(&fat32);
}

void fat32_report(void) {
    if (!fat.mounted) {
        uart_puts("no FAT32 partition mounted\n");
        return;
    }

    uart_puts("partition at block ");
    uart_dec(fat.part_lba);
    uart_puts(", FAT at ");
    uart_dec(fat.fat_start);
    uart_puts(" (");
    uart_dec(fat.num_fats);
    uart_puts(" copies of ");
    uart_dec(fat.fat_sectors);
    uart_puts(" blocks)\ndata at ");
    uart_dec(fat.data_start);
    uart_puts(", root cluster ");
    uart_dec(fat.root_cluster);
    uart_puts(", ");
    uart_dec(fat.cluster_bytes);
    uart_puts(" bytes per cluster\n");
}

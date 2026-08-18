#include "vfs.h"
#include "cpio.h"
#include "mm.h"
#include "uart.h"
#include "string.h"

/* The initial ramdisk, as a read-only filesystem.
 *
 * The archive is already in memory and reserved for the life of the system, so
 * a file's contents are the bytes in the archive -- only the tree that names
 * them is built here, once, at mount time. Everything that would change it
 * fails. */

#define IRFS_FILE 0
#define IRFS_DIR  1

struct irfs_node {
    char name[MAX_COMPONENT_LEN + 1];
    int  type;

    struct vnode *parent;

    struct vnode *entries[MAX_DIR_ENTRIES];
    int nentries;

    const char *data;                   /* into the archive, never copied */
    size_t      size;
};

static struct vnode_operations irfs_v_ops;
static struct file_operations  irfs_f_ops;

static struct irfs_node *node_of(struct vnode *v) {
    return (struct irfs_node *)v->internal;
}

static struct vnode *new_node(const char *name, int len, int type,
                              struct vnode *parent) {
    struct vnode     *v = kmalloc(sizeof(struct vnode));
    struct irfs_node *n = kmalloc(sizeof(struct irfs_node));

    if (v == 0 || n == 0) {
        if (v) kfree(v);
        if (n) kfree(n);
        return 0;
    }

    memset(n, 0, sizeof(*n));

    if (len > MAX_COMPONENT_LEN) len = MAX_COMPONENT_LEN;
    for (int i = 0; i < len; i++) n->name[i] = name[i];
    n->name[len] = '\0';

    n->type   = type;
    n->parent = parent ? parent : v;

    v->mount    = 0;
    v->v_ops    = &irfs_v_ops;
    v->f_ops    = (type == IRFS_DIR) ? 0 : &irfs_f_ops;
    v->internal = n;

    return v;
}

static struct vnode *find_child(struct vnode *dir, const char *name, int len) {
    struct irfs_node *d = node_of(dir);

    for (int i = 0; i < d->nentries; i++) {
        struct irfs_node *c = node_of(d->entries[i]);
        if (strncmp(c->name, name, len) == 0 && c->name[len] == '\0')
            return d->entries[i];
    }
    return 0;
}

static struct vnode *add_child(struct vnode *dir, const char *name, int len,
                               int type) {
    struct irfs_node *d = node_of(dir);
    if (d->nentries >= MAX_DIR_ENTRIES) return 0;

    struct vnode *v = new_node(name, len, type, dir);
    if (v == 0) return 0;

    d->entries[d->nentries++] = v;
    return v;
}

/* Places one archive entry in the tree, making the directories above it as it
 * goes: the archive lists "./dir/nested.txt" whether or not it also listed
 * "./dir", and the order it lists them in is its own business. */
static void place(struct vnode *root, const char *path, const char *data,
                  uint32_t size, int is_dir) {
    struct vnode *dir = root;
    const char *p = path;

    for (;;) {
        while (*p == '/') p++;

        /* The archive is built with `find .`, so every name starts "./". */
        if (p[0] == '.' && (p[1] == '/' || p[1] == '\0')) { p++; continue; }
        if (*p == '\0') return;                 /* the directory itself */

        int len = 0;
        while (p[len] && p[len] != '/') len++;

        int last = (p[len] == '\0');

        /* An intermediate component is a directory whatever the archive says;
         * the last one is whatever the entry's mode makes it. */
        int type = (!last || is_dir) ? IRFS_DIR : IRFS_FILE;

        struct vnode *child = find_child(dir, p, len);

        if (child == 0) {
            child = add_child(dir, p, len, type);
            if (child == 0) return;             /* directory full */
        }

        if (last) {
            struct irfs_node *n = node_of(child);
            if (n->type == IRFS_FILE) { n->data = data; n->size = size; }
            return;
        }

        dir = child;
        p += len;
    }
}

static int irfs_lookup(struct vnode *dir, struct vnode **target,
                       const char *name) {
    struct irfs_node *d = node_of(dir);

    if (d->type != IRFS_DIR) return -1;

    if (strcmp(name, ".") == 0)  { *target = dir;       return 0; }
    if (strcmp(name, "..") == 0) { *target = d->parent; return 0; }

    struct vnode *child = find_child(dir, name, (int)strlen(name));
    if (child == 0) return -1;

    *target = child;
    return 0;
}

static int irfs_readdir(struct vnode *dir, int index, char *name) {
    struct irfs_node *d = node_of(dir);

    if (d->type != IRFS_DIR || index < 0 || index >= d->nentries) return -1;

    const char *src = node_of(d->entries[index])->name;
    int i = 0;
    while (src[i]) { name[i] = src[i]; i++; }
    name[i] = '\0';
    return 0;
}

/* Read-only, so everything that would change the archive says so rather than
 * pretending to succeed. */
static int irfs_readonly_create(struct vnode *dir, struct vnode **target,
                                const char *name) {
    (void)dir; (void)target; (void)name;
    return -1;
}

static int irfs_write(struct file *file, const void *buf, size_t len) {
    (void)file; (void)buf; (void)len;
    return -1;
}

static int irfs_read(struct file *file, void *buf, size_t len) {
    struct irfs_node *n = node_of(file->vnode);

    if (n->type != IRFS_FILE || n->data == 0) return -1;
    if (file->f_pos >= n->size) return 0;

    if (file->f_pos + len > n->size) len = n->size - file->f_pos;

    memcpy(buf, n->data + file->f_pos, len);
    file->f_pos += len;
    return (int)len;
}

static int irfs_open(struct vnode *node, struct file **target) {
    (void)node; (void)target;
    return 0;
}

static int irfs_close(struct file *file) {
    (void)file;
    return 0;
}

static long irfs_lseek64(struct file *file, long offset, int whence) {
    struct irfs_node *n = node_of(file->vnode);

    if (whence != SEEK_SET || offset < 0 || (size_t)offset > n->size) return -1;

    file->f_pos = (size_t)offset;
    return offset;
}

static int irfs_setup_mount(struct filesystem *fs, struct mount *mount) {
    (void)fs;

    struct vnode *root = new_node("/", 1, IRFS_DIR, 0);
    if (root == 0) return -1;

    if (!cpio_valid()) {
        /* An empty mount point is a better answer than a failed boot. */
        uart_puts("initramfs: no archive to mount\n");
        mount->root = root;
        return 0;
    }

    const char *ptr = (const char *)cpio_get_base();
    const char *name, *data;
    uint32_t size, mode;

    while ((ptr = cpio_next(ptr, &name, &data, &size, &mode)) != 0)
        place(root, name, data, size,
              (mode & CPIO_MODE_FMT) == CPIO_MODE_DIR);

    mount->root = root;
    return 0;
}

static struct vnode_operations irfs_v_ops = {
    .lookup  = irfs_lookup,
    .create  = irfs_readonly_create,
    .mkdir   = irfs_readonly_create,
    .readdir = irfs_readdir,
};

static struct file_operations irfs_f_ops = {
    .write   = irfs_write,
    .read    = irfs_read,
    .open    = irfs_open,
    .close   = irfs_close,
    .lseek64 = irfs_lseek64,
    .ioctl   = 0,
};

static struct filesystem initramfs = {
    .name        = "initramfs",
    .setup_mount = irfs_setup_mount,
};

void initramfs_register(void) {
    register_filesystem(&initramfs);
}

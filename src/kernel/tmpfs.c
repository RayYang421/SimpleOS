#include "vfs.h"
#include "mm.h"
#include "string.h"

/* An in-memory filesystem, and the one the system is rooted on. A directory is
 * a fixed array of entries and a file is one buffer, both sized by the limits
 * the lab fixes, so nothing here needs to grow. */

#define TMPFS_FILE 0
#define TMPFS_DIR  1

struct tmpfs_node {
    char name[MAX_COMPONENT_LEN + 1];
    int  type;

    struct vnode *parent;                       /* itself, at the root */

    struct vnode *entries[MAX_DIR_ENTRIES];     /* directories */
    int nentries;

    char  *data;                                /* files */
    size_t size;
};

static struct vnode_operations tmpfs_v_ops;
static struct file_operations  tmpfs_f_ops;

/* A directory has no file operations, which is also how the layer above tells
 * the two apart: a directory cannot be opened, and only a directory can be
 * stood in. */
static struct vnode *new_node(const char *name, int type, struct vnode *parent) {
    struct vnode      *v = kmalloc(sizeof(struct vnode));
    struct tmpfs_node *n = kmalloc(sizeof(struct tmpfs_node));

    if (v == 0 || n == 0) {
        if (v) kfree(v);
        if (n) kfree(n);
        return 0;
    }

    memset(n, 0, sizeof(*n));

    int i = 0;
    while (name[i] && i < MAX_COMPONENT_LEN) { n->name[i] = name[i]; i++; }
    n->name[i] = '\0';

    n->type   = type;
    n->parent = parent ? parent : v;

    v->mount    = 0;
    v->v_ops    = &tmpfs_v_ops;
    v->f_ops    = (type == TMPFS_DIR) ? 0 : &tmpfs_f_ops;
    v->internal = n;

    return v;
}

static struct tmpfs_node *node_of(struct vnode *v) {
    return (struct tmpfs_node *)v->internal;
}

static int tmpfs_lookup(struct vnode *dir, struct vnode **target,
                        const char *name) {
    struct tmpfs_node *d = node_of(dir);

    if (d->type != TMPFS_DIR) return -1;

    if (strcmp(name, ".") == 0)  { *target = dir;       return 0; }
    if (strcmp(name, "..") == 0) { *target = d->parent; return 0; }

    for (int i = 0; i < d->nentries; i++) {
        if (strcmp(node_of(d->entries[i])->name, name) == 0) {
            *target = d->entries[i];
            return 0;
        }
    }

    return -1;
}

static int add_entry(struct vnode *dir, struct vnode **target,
                     const char *name, int type) {
    struct tmpfs_node *d = node_of(dir);
    struct vnode *existing;

    if (d->type != TMPFS_DIR) return -1;
    if (d->nentries >= MAX_DIR_ENTRIES) return -1;
    if (strlen(name) > MAX_COMPONENT_LEN) return -1;
    if (tmpfs_lookup(dir, &existing, name) == 0) return -1;   /* already there */

    struct vnode *v = new_node(name, type, dir);
    if (v == 0) return -1;

    d->entries[d->nentries++] = v;
    *target = v;
    return 0;
}

static int tmpfs_create(struct vnode *dir, struct vnode **target,
                        const char *name) {
    return add_entry(dir, target, name, TMPFS_FILE);
}

static int tmpfs_mkdir(struct vnode *dir, struct vnode **target,
                       const char *name) {
    return add_entry(dir, target, name, TMPFS_DIR);
}

static int tmpfs_readdir(struct vnode *dir, int index, char *name) {
    struct tmpfs_node *d = node_of(dir);

    if (d->type != TMPFS_DIR || index < 0 || index >= d->nentries) return -1;

    const char *src = node_of(d->entries[index])->name;
    int i = 0;
    while (src[i]) { name[i] = src[i]; i++; }
    name[i] = '\0';
    return 0;
}

/* The buffer is allocated on the first write rather than with the node, so an
 * empty file costs nothing. */
static int tmpfs_write(struct file *file, const void *buf, size_t len) {
    struct tmpfs_node *n = node_of(file->vnode);

    if (n->type != TMPFS_FILE) return -1;

    if (n->data == 0) {
        n->data = kmalloc(MAX_FILE_SIZE);
        if (n->data == 0) return -1;
    }

    if (file->f_pos >= MAX_FILE_SIZE) return 0;
    if (file->f_pos + len > MAX_FILE_SIZE) len = MAX_FILE_SIZE - file->f_pos;

    memcpy(n->data + file->f_pos, buf, len);
    file->f_pos += len;

    /* Writing past the end is what makes a file longer; writing over the
     * middle of one does not. */
    if (file->f_pos > n->size) n->size = file->f_pos;

    return (int)len;
}

static int tmpfs_read(struct file *file, void *buf, size_t len) {
    struct tmpfs_node *n = node_of(file->vnode);

    if (n->type != TMPFS_FILE) return -1;
    if (n->data == 0 || file->f_pos >= n->size) return 0;   /* at the end */

    if (file->f_pos + len > n->size) len = n->size - file->f_pos;

    memcpy(buf, n->data + file->f_pos, len);
    file->f_pos += len;
    return (int)len;
}

static int tmpfs_open(struct vnode *node, struct file **target) {
    (void)node;
    (void)target;
    return 0;
}

static int tmpfs_close(struct file *file) {
    (void)file;
    return 0;
}

static long tmpfs_lseek64(struct file *file, long offset, int whence) {
    if (whence != SEEK_SET || offset < 0 || offset > MAX_FILE_SIZE) return -1;

    file->f_pos = (size_t)offset;
    return offset;
}

static int tmpfs_setup_mount(struct filesystem *fs, struct mount *mount) {
    (void)fs;

    struct vnode *root = new_node("/", TMPFS_DIR, 0);
    if (root == 0) return -1;

    mount->root = root;
    return 0;
}

static struct vnode_operations tmpfs_v_ops = {
    .lookup  = tmpfs_lookup,
    .create  = tmpfs_create,
    .mkdir   = tmpfs_mkdir,
    .readdir = tmpfs_readdir,
};

static struct file_operations tmpfs_f_ops = {
    .write   = tmpfs_write,
    .read    = tmpfs_read,
    .open    = tmpfs_open,
    .close   = tmpfs_close,
    .lseek64 = tmpfs_lseek64,
    .ioctl   = 0,
};

static struct filesystem tmpfs = {
    .name        = "tmpfs",
    .setup_mount = tmpfs_setup_mount,
};

void tmpfs_register(void) {
    register_filesystem(&tmpfs);
}

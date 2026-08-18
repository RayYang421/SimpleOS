#include "vfs.h"
#include "sched.h"
#include "mm.h"
#include "uart.h"
#include "string.h"

/* The filesystem registry and the mounts currently in the tree. Both are
 * lists: there are a handful of each, and walking one is never on a hot
 * path. */
static struct filesystem *fs_list;
static struct mount      *mount_list;
static struct mount       root_mount;

/* Devices register their file operations once and are handed an id; a device
 * file is a vnode that borrows the operations back. The name is kept for the
 * sake of a diagnostic, not for lookup -- a device file is found by its path
 * like any other. */
#define MAX_DEVICES 8

static struct {
    const char *name;
    struct file_operations *f_ops;
} devices[MAX_DEVICES];

static int device_count;

int register_filesystem(struct filesystem *fs) {
    if (fs == 0 || fs->name == 0 || fs->setup_mount == 0) return -1;

    for (struct filesystem *f = fs_list; f; f = f->next) {
        if (strcmp(f->name, fs->name) == 0) return -1;
    }

    fs->next = fs_list;
    fs_list  = fs;
    return 0;
}

static struct filesystem *find_filesystem(const char *name) {
    for (struct filesystem *f = fs_list; f; f = f->next) {
        if (strcmp(f->name, name) == 0) return f;
    }
    return 0;
}

struct vnode *vfs_root(void) { return root_mount.root; }

struct vnode *vfs_cwd(void) {
    struct thread *t = current();
    if (t && t->cwd) return t->cwd;
    return vfs_root();
}

/* --- path resolution -------------------------------------------------------- */

/* A vnode with a filesystem mounted on it stands for that filesystem's root.
 * Looping covers a mount on top of a mount root. */
static struct vnode *follow_mount(struct vnode *v) {
    while (v && v->mount) v = v->mount->root;
    return v;
}

static struct mount *mount_rooted_at(struct vnode *v) {
    for (struct mount *m = mount_list; m; m = m->next) {
        if (m->root == v) return m;
    }
    return 0;
}

static void name_copy(char *dst, const char *src) {
    int i = 0;
    while (src[i] && i < MAX_COMPONENT_LEN) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* Peels the next component off a path. Returns 1 for a component, 0 at the
 * end, -1 if a component is longer than a name may be. */
static int next_component(const char **p, char *out) {
    while (**p == '/') (*p)++;
    if (**p == '\0') return 0;

    int n = 0;
    while (**p && **p != '/') {
        if (n >= MAX_COMPONENT_LEN) return -1;
        out[n++] = **p;
        (*p)++;
    }

    out[n] = '\0';
    return 1;
}

/* Moves one component down from *v. */
static int step(struct vnode **v, const char *name) {
    struct vnode *cur = *v;

    if (name[0] == '.' && name[1] == '\0') return 0;

    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') {
        /* At the root of a mount, the parent lives on the other side of the
         * join, so cross back before asking. The root filesystem's root covers
         * nothing, and its own ".." is itself -- which is what makes "/.."
         * behave like ".". */
        struct mount *m = mount_rooted_at(cur);
        if (m && m->covered) cur = m->covered;
    }

    if (cur->v_ops == 0 || cur->v_ops->lookup == 0) return -1;

    struct vnode *next;
    if (cur->v_ops->lookup(cur, &next, name) != 0) return -1;

    *v = follow_mount(next);
    return 0;
}

/* Walks a path to the vnode it names. With want_parent set it stops one
 * component short and hands back that component instead, which is what create
 * and mkdir need: the thing being named does not exist yet. */
static int resolve(const char *path, struct vnode **out, int want_parent,
                   char *last) {
    if (path == 0 || strlen(path) > MAX_PATH_LEN) return -1;

    struct vnode *v = (path[0] == '/') ? vfs_root() : vfs_cwd();
    if (v == 0) return -1;
    v = follow_mount(v);

    char comp[MAX_COMPONENT_LEN + 1];
    char held[MAX_COMPONENT_LEN + 1];
    int  holding = 0;
    const char *p = path;

    for (;;) {
        int r = next_component(&p, comp);
        if (r < 0) return -1;
        if (r == 0) break;

        if (!want_parent) {
            if (step(&v, comp) != 0) return -1;
            continue;
        }

        /* One component behind, so whatever is left at the end is the name. */
        if (holding && step(&v, held) != 0) return -1;
        name_copy(held, comp);
        holding = 1;
    }

    if (want_parent) {
        if (!holding) return -1;        /* "/" names no component */
        name_copy(last, held);
    }

    *out = v;
    return 0;
}

int vfs_lookup(const char *pathname, struct vnode **target) {
    return resolve(pathname, target, 0, 0);
}

/* --- files ------------------------------------------------------------------ */

int vfs_open(const char *pathname, int flags, struct file **target) {
    struct vnode *v;

    if (resolve(pathname, &v, 0, 0) != 0) {
        if (!(flags & O_CREAT)) return -1;

        char name[MAX_COMPONENT_LEN + 1];
        struct vnode *dir;

        if (resolve(pathname, &dir, 1, name) != 0) return -1;
        if (dir->v_ops == 0 || dir->v_ops->create == 0) return -1;
        if (dir->v_ops->create(dir, &v, name) != 0) return -1;
    }

    if (v->f_ops == 0) return -1;       /* a directory, or nothing openable */

    struct file *f = kmalloc(sizeof(struct file));
    if (f == 0) return -1;

    f->vnode = v;
    f->f_pos = 0;
    f->f_ops = v->f_ops;
    f->flags = flags;

    /* The filesystem gets the last word: a device may want its own state on
     * the handle, and this is where it puts it. */
    if (f->f_ops->open && f->f_ops->open(v, &f) != 0) {
        kfree(f);
        return -1;
    }

    *target = f;
    return 0;
}

int vfs_close(struct file *file) {
    if (file == 0) return -1;

    if (file->f_ops && file->f_ops->close) file->f_ops->close(file);
    kfree(file);
    return 0;
}

int vfs_write(struct file *file, const void *buf, size_t len) {
    if (file == 0 || file->f_ops == 0 || file->f_ops->write == 0) return -1;
    return file->f_ops->write(file, buf, len);
}

int vfs_read(struct file *file, void *buf, size_t len) {
    if (file == 0 || file->f_ops == 0 || file->f_ops->read == 0) return -1;
    return file->f_ops->read(file, buf, len);
}

long vfs_lseek64(struct file *file, long offset, int whence) {
    if (file == 0 || file->f_ops == 0 || file->f_ops->lseek64 == 0) return -1;
    return file->f_ops->lseek64(file, offset, whence);
}

int vfs_ioctl(struct file *file, unsigned long request, void *arg) {
    if (file == 0 || file->f_ops == 0 || file->f_ops->ioctl == 0) return -1;
    return file->f_ops->ioctl(file, request, arg);
}

int vfs_mkdir(const char *pathname) {
    char name[MAX_COMPONENT_LEN + 1];
    struct vnode *dir, *made;

    if (resolve(pathname, &dir, 1, name) != 0) return -1;
    if (dir->v_ops == 0 || dir->v_ops->mkdir == 0) return -1;

    return dir->v_ops->mkdir(dir, &made, name);
}

int vfs_mount(const char *target, const char *filesystem) {
    struct filesystem *fs = find_filesystem(filesystem);
    if (fs == 0) return -1;

    struct vnode *at;
    if (resolve(target, &at, 0, 0) != 0) return -1;
    if (at->mount) return -1;               /* already covered */

    struct mount *m = kmalloc(sizeof(struct mount));
    if (m == 0) return -1;

    m->fs      = fs;
    m->root    = 0;
    m->covered = at;

    if (fs->setup_mount(fs, m) != 0 || m->root == 0) {
        kfree(m);
        return -1;
    }

    /* Covering the vnode is what makes every later lookup land in the new
     * filesystem instead. */
    at->mount = m;

    m->next    = mount_list;
    mount_list = m;
    return 0;
}

/* --- devices ---------------------------------------------------------------- */

int register_device(const char *name, struct file_operations *f_ops) {
    if (device_count >= MAX_DEVICES || f_ops == 0) return -1;

    devices[device_count].name  = name;
    devices[device_count].f_ops = f_ops;
    return device_count++;
}

int vfs_mknod(const char *pathname, int device_id) {
    if (device_id < 0 || device_id >= device_count) return -1;

    char name[MAX_COMPONENT_LEN + 1];
    struct vnode *dir, *node;

    if (resolve(pathname, &dir, 1, name) != 0) return -1;
    if (dir->v_ops == 0 || dir->v_ops->create == 0) return -1;
    if (dir->v_ops->create(dir, &node, name) != 0) return -1;

    /* The file exists in whatever filesystem holds the directory; only its
     * operations come from the device. */
    node->f_ops = devices[device_id].f_ops;
    return 0;
}

/* --- current directory and file descriptors --------------------------------- */

int vfs_chdir(const char *pathname) {
    struct vnode *v;
    struct thread *t = current();

    if (t == 0) return -1;
    if (resolve(pathname, &v, 0, 0) != 0) return -1;

    /* Only a directory can be stood in, and a directory is the thing with no
     * file operations of its own. */
    if (v->f_ops != 0) return -1;

    t->cwd = v;
    return 0;
}

int fd_install(struct file *file) {
    struct thread *t = current();
    if (t == 0 || file == 0) return -1;

    for (int i = 0; i < MAX_FD; i++) {
        if (t->fds[i] == 0) { t->fds[i] = file; return i; }
    }
    return -1;                              /* the table is full */
}

struct file *fd_get(int fd) {
    struct thread *t = current();
    if (t == 0 || fd < 0 || fd >= MAX_FD) return 0;
    return t->fds[fd];
}

int fd_close(int fd) {
    struct thread *t = current();
    if (t == 0 || fd < 0 || fd >= MAX_FD || t->fds[fd] == 0) return -1;

    vfs_close(t->fds[fd]);
    t->fds[fd] = 0;
    return 0;
}

/* The child gets handles of its own rather than sharing the parent's, so
 * closing one or seeking in it leaves the other alone. That is not what POSIX
 * says -- there the two share one file description, and one process's seek
 * moves the other's -- but sharing would need the handles reference counted,
 * and nothing here wants the POSIX behaviour. */
void fd_table_copy(struct thread *child, struct thread *parent) {
    child->cwd = parent->cwd;

    for (int i = 0; i < MAX_FD; i++) {
        child->fds[i] = 0;
        if (parent->fds[i] == 0) continue;

        struct file *f = kmalloc(sizeof(struct file));
        if (f == 0) continue;

        *f = *parent->fds[i];
        child->fds[i] = f;
    }
}

void fd_table_close_all(struct thread *t) {
    if (t == 0) return;

    for (int i = 0; i < MAX_FD; i++) {
        if (t->fds[i]) {
            vfs_close(t->fds[i]);
            t->fds[i] = 0;
        }
    }
}

void fd_table_open_stdio(void) {
    struct file *f;

    /* Three separate opens rather than one handle in three slots: each carries
     * its own position, and closing one must not close the others. */
    for (int i = 0; i < 3; i++) {
        if (vfs_open("/dev/uart", 0, &f) != 0) return;
        if (fd_install(f) != i) { vfs_close(f); return; }
    }
}

/* --- startup ---------------------------------------------------------------- */

void tmpfs_register(void);          /* tmpfs.c */
void initramfs_register(void);      /* initramfs_fs.c */
void dev_uart_register(void);       /* dev_uart.c */
void dev_fb_register(void);         /* dev_fb.c */

void vfs_init(void) {
    tmpfs_register();
    initramfs_register();

    struct filesystem *rootfs = find_filesystem("tmpfs");
    if (rootfs == 0) {
        uart_puts("vfs: no tmpfs to use as the root filesystem\n");
        return;
    }

    /* Mounted by hand: vfs_mount would have to look the mount point up, and
     * there is no tree to look anything up in yet. */
    root_mount.fs      = rootfs;
    root_mount.covered = 0;
    if (rootfs->setup_mount(rootfs, &root_mount) != 0) {
        uart_puts("vfs: could not mount the root filesystem\n");
        return;
    }

    root_mount.next = mount_list;
    mount_list      = &root_mount;

    if (vfs_mkdir("/initramfs") != 0 ||
        vfs_mount("/initramfs", "initramfs") != 0)
        uart_puts("vfs: could not mount the initramfs\n");

    if (vfs_mkdir("/dev") != 0) {
        uart_puts("vfs: could not create /dev\n");
        return;
    }

    dev_uart_register();
    dev_fb_register();
}

/* --- reporting -------------------------------------------------------------- */

void vfs_report(const char *path) {
    struct vnode *v;

    if (resolve(path, &v, 0, 0) != 0) {
        uart_puts("no such path: ");
        uart_puts(path);
        uart_puts("\n");
        return;
    }

    if (v->f_ops) {
        uart_puts(path);
        uart_puts(" is a file\n");
        return;
    }

    if (v->v_ops == 0 || v->v_ops->readdir == 0) {
        uart_puts("this filesystem cannot list a directory\n");
        return;
    }

    char name[MAX_COMPONENT_LEN + 1];

    for (int i = 0; v->v_ops->readdir(v, i, name) == 0; i++) {
        struct vnode *child;

        uart_puts("  ");
        uart_puts(name);

        /* A directory is the thing with no file operations of its own, which
         * is what the layer above uses to tell them apart too. */
        if (v->v_ops->lookup(v, &child, name) == 0) {
            if (child->mount)       uart_puts("/   (mount point)");
            else if (!child->f_ops) uart_puts("/");
        }
        uart_puts("\n");
    }
}

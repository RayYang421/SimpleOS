#ifndef VFS_H
#define VFS_H

#include "types.h"

/* Limits the lab fixes. They are small on purpose: a directory is a flat array
 * and a file is one buffer, so nothing here needs an allocator of its own. */
#define MAX_PATH_LEN      255
#define MAX_COMPONENT_LEN 15
#define MAX_DIR_ENTRIES   16
#define MAX_FILE_SIZE     4096
#define MAX_FD            16

/* open() flags. Only O_CREAT means anything here; the rest of the POSIX set
 * would need permissions and truncation to be worth defining. */
#define O_CREAT 00000100

/* lseek64() whence. */
#define SEEK_SET 0

/* ioctl() on /dev/framebuffer. */
#define FBIOGET_INFO 0

struct vnode;
struct file;
struct mount;
struct filesystem;

struct vnode {
    /* Non-null when a filesystem is mounted *on* this vnode: a lookup that
     * lands here continues at mount->root instead. */
    struct mount *mount;

    struct vnode_operations *v_ops;
    struct file_operations  *f_ops;
    void *internal;                 /* whatever the filesystem keeps per node */
};

struct file {
    struct vnode *vnode;
    size_t f_pos;
    struct file_operations *f_ops;
    int flags;
};

struct mount {
    struct vnode *root;
    struct filesystem *fs;

    /* The vnode this was mounted on, so ".." out of a mount root can get back
     * over the join. Null for the root filesystem, which has nothing above it. */
    struct vnode *covered;

    struct mount *next;             /* every mount in the tree */
};

struct filesystem {
    const char *name;
    int (*setup_mount)(struct filesystem *fs, struct mount *mount);
    struct filesystem *next;        /* the registry is a list */
};

struct file_operations {
    int  (*write)(struct file *file, const void *buf, size_t len);
    int  (*read)(struct file *file, void *buf, size_t len);
    int  (*open)(struct vnode *file_node, struct file **target);
    int  (*close)(struct file *file);
    long (*lseek64)(struct file *file, long offset, int whence);
    int  (*ioctl)(struct file *file, unsigned long request, void *arg);
};

struct vnode_operations {
    int (*lookup)(struct vnode *dir_node, struct vnode **target,
                  const char *component_name);
    int (*create)(struct vnode *dir_node, struct vnode **target,
                  const char *component_name);
    int (*mkdir)(struct vnode *dir_node, struct vnode **target,
                 const char *component_name);

    /* Extension. The lab's interface has no way to enumerate a directory,
     * which leaves no way to write an `ls`. Fills name with entry `index` and
     * returns 0, or returns -1 past the last one. */
    int (*readdir)(struct vnode *dir_node, int index, char *name);
};

/* --- the interface the lab asks for --- */
int register_filesystem(struct filesystem *fs);
int vfs_open(const char *pathname, int flags, struct file **target);
int vfs_close(struct file *file);
int vfs_write(struct file *file, const void *buf, size_t len);
int vfs_read(struct file *file, void *buf, size_t len);
int vfs_mkdir(const char *pathname);
int vfs_mount(const char *target, const char *filesystem);
int vfs_lookup(const char *pathname, struct vnode **target);

long vfs_lseek64(struct file *file, long offset, int whence);
int  vfs_ioctl(struct file *file, unsigned long request, void *arg);

/* Builds the tree the system starts with: tmpfs at /, the initramfs at
 * /initramfs, and the device files under /dev. */
void vfs_init(void);

/* Mounts the root filesystem directly, before there is a tree to look a mount
 * point up in. */
struct vnode *vfs_root(void);

/* --- devices ---
 *
 * A device registers its file operations once and gets an id back; a device
 * file is then a vnode in some directory that borrows those operations. */
int register_device(const char *name, struct file_operations *f_ops);
int vfs_mknod(const char *pathname, int device_id);

/* --- current directory and file descriptors ---
 *
 * Both belong to the thread, so a process's idea of "here" and of what fd 3
 * means are its own. */
struct vnode *vfs_cwd(void);
int  vfs_chdir(const char *pathname);
int  fd_install(struct file *file);
struct file *fd_get(int fd);
int  fd_close(int fd);

/* Called from fork and from thread teardown. */
struct thread;
void fd_table_copy(struct thread *child, struct thread *parent);
void fd_table_close_all(struct thread *t);

/* Opens /dev/uart three times, for stdin, stdout and stderr. */
void fd_table_open_stdio(void);

void vfs_report(const char *path);

/* Framebuffer geometry, as ioctl reports it. */
struct framebuffer_info {
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    unsigned int isrgb;
};

#endif

#include "vfs.h"
#include "uart.h"

/* /dev/uart. Reading and writing it is reading and writing the console, which
 * is what lets a process treat its terminal as a file -- and what makes fds 0,
 * 1 and 2 mean something. */

static int uart_dev_write(struct file *file, const void *buf, size_t len) {
    (void)file;
    uart_write((const char *)buf, len);
    return (int)len;
}

static int uart_dev_read(struct file *file, void *buf, size_t len) {
    char *out = (char *)buf;
    (void)file;

    /* Blocks until the whole request is satisfied, the same as the uart_read
     * syscall does; a terminal has no end of file to stop at. */
    for (size_t i = 0; i < len; i++) out[i] = uart_getc();
    return (int)len;
}

static int uart_dev_open(struct vnode *node, struct file **target) {
    (void)node; (void)target;
    return 0;
}

static int uart_dev_close(struct file *file) {
    (void)file;
    return 0;
}

/* A character device has no position to seek to. */
static struct file_operations uart_f_ops = {
    .write   = uart_dev_write,
    .read    = uart_dev_read,
    .open    = uart_dev_open,
    .close   = uart_dev_close,
    .lseek64 = 0,
    .ioctl   = 0,
};

void dev_uart_register(void) {
    int id = register_device("uart", &uart_f_ops);
    if (id < 0) return;

    vfs_mknod("/dev/uart", id);
}

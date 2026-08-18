#include "vfs.h"
#include "mbox.h"
#include "mmu.h"
#include "uart.h"
#include "string.h"

/* /dev/framebuffer. Write-only: bytes written land in the framebuffer at the
 * file position, so drawing is seeking to a pixel and writing its colour.
 * ioctl reports the geometry, since a program cannot work out where a pixel
 * lives without the pitch. */

#define FB_WIDTH  1024
#define FB_HEIGHT 768
#define FB_DEPTH  32

#define TAG_SET_PHYSICAL_SIZE 0x00048003
#define TAG_SET_VIRTUAL_SIZE  0x00048004
#define TAG_SET_VIRTUAL_OFF   0x00048009
#define TAG_SET_DEPTH         0x00048005
#define TAG_SET_PIXEL_ORDER   0x00048006
#define TAG_ALLOCATE_BUFFER   0x00040001
#define TAG_GET_PITCH         0x00040008
#define TAG_LAST              0x00000000

static unsigned char *fb_base;      /* through the kernel mapping */
static unsigned int   fb_size;
static struct framebuffer_info fb_info;

/* One mailbox message asks for everything at once: the VideoCore only
 * allocates a buffer once the geometry is settled, so splitting this up would
 * mean allocating before knowing what to allocate. */
static int fb_setup(void) {
    static volatile unsigned int mbox[36] __attribute__((aligned(16)));

    mbox[0]  = 35 * 4;
    mbox[1]  = 0;                       /* request */

    mbox[2]  = TAG_SET_PHYSICAL_SIZE;
    mbox[3]  = 8; mbox[4] = 8;
    mbox[5]  = FB_WIDTH;
    mbox[6]  = FB_HEIGHT;

    mbox[7]  = TAG_SET_VIRTUAL_SIZE;
    mbox[8]  = 8; mbox[9] = 8;
    mbox[10] = FB_WIDTH;
    mbox[11] = FB_HEIGHT;

    mbox[12] = TAG_SET_VIRTUAL_OFF;
    mbox[13] = 8; mbox[14] = 8;
    mbox[15] = 0; mbox[16] = 0;

    mbox[17] = TAG_SET_DEPTH;
    mbox[18] = 4; mbox[19] = 4;
    mbox[20] = FB_DEPTH;

    mbox[21] = TAG_SET_PIXEL_ORDER;
    mbox[22] = 4; mbox[23] = 4;
    mbox[24] = 1;                       /* RGB rather than BGR */

    mbox[25] = TAG_ALLOCATE_BUFFER;
    mbox[26] = 8; mbox[27] = 8;
    mbox[28] = 4096;                    /* alignment going in, address coming out */
    mbox[29] = 0;

    mbox[30] = TAG_GET_PITCH;
    mbox[31] = 4; mbox[32] = 4;
    mbox[33] = 0;

    mbox[34] = TAG_LAST;

    if (!mbox_call(8, (unsigned int *)mbox)) return -1;
    if (mbox[20] != FB_DEPTH || mbox[28] == 0) return -1;

    /* The VideoCore answers with a bus address; the ARM side sees it without
     * the alias bits. */
    uint64_t phys = mbox[28] & 0x3FFFFFFF;

    fb_info.width  = mbox[5];
    fb_info.height = mbox[6];
    fb_info.pitch  = mbox[33];
    fb_info.isrgb  = mbox[24];

    fb_size = mbox[29];
    fb_base = (unsigned char *)VA(phys);
    return 0;
}

static int fb_write(struct file *file, const void *buf, size_t len) {
    if (fb_base == 0) return -1;
    if (file->f_pos >= fb_size) return 0;

    if (file->f_pos + len > fb_size) len = fb_size - file->f_pos;

    memcpy(fb_base + file->f_pos, buf, len);
    file->f_pos += len;
    return (int)len;
}

/* Write-only, as the lab specifies. */
static int fb_read(struct file *file, void *buf, size_t len) {
    (void)file; (void)buf; (void)len;
    return -1;
}

static int fb_open(struct vnode *node, struct file **target) {
    (void)node; (void)target;
    return fb_base ? 0 : -1;
}

static int fb_close(struct file *file) {
    (void)file;
    return 0;
}

static long fb_lseek64(struct file *file, long offset, int whence) {
    if (whence != SEEK_SET || offset < 0 || (unsigned int)offset > fb_size)
        return -1;

    file->f_pos = (size_t)offset;
    return offset;
}

static int fb_ioctl(struct file *file, unsigned long request, void *arg) {
    (void)file;

    if (request != FBIOGET_INFO || arg == 0 || fb_base == 0) return -1;

    *(struct framebuffer_info *)arg = fb_info;
    return 0;
}

static struct file_operations fb_f_ops = {
    .write   = fb_write,
    .read    = fb_read,
    .open    = fb_open,
    .close   = fb_close,
    .lseek64 = fb_lseek64,
    .ioctl   = fb_ioctl,
};

void dev_fb_register(void) {
    if (fb_setup() != 0) {
        /* The file is still created: a program that opens it gets a clean
         * failure rather than a missing path. */
        uart_puts("framebuffer: the mailbox would not allocate one\n");
    } else {
        uart_puts("framebuffer: ");
        uart_dec(fb_info.width);
        uart_puts("x");
        uart_dec(fb_info.height);
        uart_puts(", pitch ");
        uart_dec(fb_info.pitch);
        uart_puts(", at ");
        uart_hex(PA(fb_base));
        uart_puts("\n");
    }

    int id = register_device("framebuffer", &fb_f_ops);
    if (id < 0) return;

    vfs_mknod("/dev/framebuffer", id);
}

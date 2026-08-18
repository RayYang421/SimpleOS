/* File system demonstration, all of it through system calls: the descriptor
 * table, the current directory, a mount, the read-only initramfs, and the two
 * device files.
 *
 * Same relocatability rules as hello.c -- no globals, nothing absolute. */

#define SYS_UART_WRITE  2
#define SYS_EXIT        5
#define SYS_OPEN        11
#define SYS_CLOSE       12
#define SYS_FWRITE      13
#define SYS_FREAD       14
#define SYS_MKDIR       15
#define SYS_MOUNT       16
#define SYS_CHDIR       17
#define SYS_LSEEK64     18
#define SYS_IOCTL       19

#define O_CREAT   00000100
#define SEEK_SET  0
#define STDOUT    1

struct framebuffer_info {
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    unsigned int isrgb;
};

static long sys3(long n, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc 0"
                     : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}

static long sys5(long n, long a0, long a1, long a2, long a3, long a4) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    register long x4 __asm__("x4") = a4;
    __asm__ volatile("svc 0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4)
                     : "memory", "cc");
    return x0;
}

static int put_str(char *out, int at, const char *s) {
    while (*s) out[at++] = *s++;
    return at;
}

static int put_dec(char *out, int at, long v) {
    char digits[20];
    int i = 0;

    if (v == 0) { out[at++] = '0'; return at; }
    if (v < 0)  { out[at++] = '-'; v = -v; }

    while (v > 0) { digits[i++] = (char)('0' + (v % 10)); v /= 10; }
    while (i > 0) out[at++] = digits[--i];
    return at;
}

/* Everything after the first line goes out through descriptor 1, which the
 * kernel opened on /dev/uart -- writing to the console is writing to a file. */
static void emit(const char *buf, int n) {
    sys3(SYS_FWRITE, STDOUT, (long)buf, n);
}

static void say(const char *s) {
    char buf[128];
    int n = put_str(buf, 0, s);
    buf[n++] = '\n';
    emit(buf, n);
}

static void line(const char *a, long v, const char *b) {
    char buf[128];
    int n = put_str(buf, 0, a);
    n = put_dec(buf, n, v);
    n = put_str(buf, n, b);
    buf[n++] = '\n';
    emit(buf, n);
}

static void line_str(const char *a, const char *b) {
    char buf[128];
    int n = put_str(buf, 0, a);
    n = put_str(buf, n, b);
    buf[n++] = '\n';
    emit(buf, n);
}

__attribute__((section(".text.entry")))
void _start(void) {
    char buf[64];

    /* Through the old uart_write call, so that a broken descriptor table shows
     * up as silence from here on rather than silence throughout. */
    const char *banner = "user: fs demo starting\n";
    int n = 0;
    while (banner[n]) n++;
    sys3(SYS_UART_WRITE, (long)banner, n, 0);

    say("user: writing this through fd 1, which is /dev/uart");

    /* A file in the root filesystem: create it, write it, close it, and read
     * it back through a second descriptor. */
    sys3(SYS_MKDIR, (long)"/home", 0, 0);

    long fd = sys3(SYS_OPEN, (long)"/home/note.txt", O_CREAT, 0);
    line("user: open for writing gave fd ", fd, "");
    sys3(SYS_FWRITE, fd, (long)"hello from a user process", 25);
    sys3(SYS_CLOSE, fd, 0, 0);

    fd = sys3(SYS_OPEN, (long)"/home/note.txt", 0, 0);
    n = (int)sys3(SYS_FREAD, fd, (long)buf, 63);
    buf[n < 0 ? 0 : n] = '\0';
    line_str("user: read it back: ", buf);
    sys3(SYS_CLOSE, fd, 0, 0);

    /* The current directory makes a relative path mean something. */
    sys3(SYS_CHDIR, (long)"/home", 0, 0);
    fd = sys3(SYS_OPEN, (long)"note.txt", 0, 0);
    line("user: the same file by relative path, fd ", fd, "");
    sys3(SYS_CLOSE, fd, 0, 0);

    /* Mounting puts a different filesystem behind an existing directory. */
    sys3(SYS_MKDIR, (long)"/home/mnt", 0, 0);
    long r = sys5(SYS_MOUNT, 0, (long)"/home/mnt", (long)"tmpfs", 0, 0);
    line("user: mount tmpfs on /home/mnt returned ", r, "");

    fd = sys3(SYS_OPEN, (long)"/home/mnt/inside.txt", O_CREAT, 0);
    line("user: a file in the mounted filesystem, fd ", fd, "");
    sys3(SYS_CLOSE, fd, 0, 0);

    /* The initramfs is mounted too, and is read-only. */
    fd = sys3(SYS_OPEN, (long)"/initramfs/hello.txt", 0, 0);
    n = (int)sys3(SYS_FREAD, fd, (long)buf, 20);
    buf[n < 0 ? 0 : n] = '\0';
    line_str("user: /initramfs/hello.txt begins: ", buf);

    /* Seeking, then reading from the new position. */
    sys3(SYS_LSEEK64, fd, 6, SEEK_SET);
    n = (int)sys3(SYS_FREAD, fd, (long)buf, 14);
    buf[n < 0 ? 0 : n] = '\0';
    line_str("user: after seeking to 6: ", buf);
    sys3(SYS_CLOSE, fd, 0, 0);

    r = sys3(SYS_OPEN, (long)"/initramfs/new.txt", O_CREAT, 0);
    line("user: creating a file in the initramfs returned ", r,
         " (it is read-only)");

    /* The framebuffer: ask it how it is laid out, then colour a pixel. */
    fd = sys3(SYS_OPEN, (long)"/dev/framebuffer", 0, 0);

    if (fd >= 0) {
        struct framebuffer_info info;

        if (sys3(SYS_IOCTL, fd, 0, (long)&info) == 0) {
            line("user: framebuffer is ", info.width, " wide");
            line("user: framebuffer is ", info.height, " tall");
            line("user: framebuffer pitch is ", info.pitch, " bytes");

            /* One pixel at (2, 1): the row is a pitch apart, the column four
             * bytes. */
            unsigned int pixel = 0x00FF7F00;
            sys3(SYS_LSEEK64, fd, (long)(info.pitch + 2 * 4), SEEK_SET);
            r = sys3(SYS_FWRITE, fd, (long)&pixel, 4);
            line("user: wrote ", r, " bytes of pixel to the framebuffer");
        }
        sys3(SYS_CLOSE, fd, 0, 0);
    }

    say("user: fs demo done");
    sys3(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}

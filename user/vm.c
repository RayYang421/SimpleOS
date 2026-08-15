/* Virtual memory demonstration: mmap, demand paging, copy-on-write, and a
 * write to a read-only region, which is where the process ends.
 *
 * Same relocatability rules as hello.c -- no globals, nothing absolute. */

#define SYS_GETPID      0
#define SYS_UART_WRITE  2
#define SYS_FORK        4
#define SYS_EXIT        5
#define SYS_MBOX_CALL   6
#define SYS_MMAP        10

#define MBOX_CH_PROP           8
#define MBOX_TAG_BOARD_REVISION 0x00010002

#define PROT_NONE       0
#define PROT_READ       1
#define PROT_WRITE      2
#define PROT_EXEC       4

#define MAP_ANONYMOUS   0x20
#define MAP_POPULATE    0x8000

static long sys(long n, long a0, long a1) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory", "cc");
    return x0;
}

static void *sys_mmap(long addr, long len, long prot, long flags) {
    register long x8 __asm__("x8") = SYS_MMAP;
    register long x0 __asm__("x0") = addr;
    register long x1 __asm__("x1") = len;
    register long x2 __asm__("x2") = prot;
    register long x3 __asm__("x3") = flags;
    __asm__ volatile("svc 0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                     : "memory", "cc");
    return (void *)x0;
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

static int put_hex(char *out, int at, unsigned long v) {
    at = put_str(out, at, "0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned d = (unsigned)((v >> shift) & 0xF);
        out[at++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    return at;
}

static void line(const char *a, long v, const char *b) {
    char buf[96];
    int n = put_str(buf, 0, a);
    n = put_dec(buf, n, v);
    n = put_str(buf, n, b);
    buf[n++] = '\n';
    sys(SYS_UART_WRITE, (long)buf, n);
}

static void line_hex(const char *a, unsigned long v) {
    char buf[96];
    int n = put_str(buf, 0, a);
    n = put_hex(buf, n, v);
    buf[n++] = '\n';
    sys(SYS_UART_WRITE, (long)buf, n);
}

static void say(const char *s) {
    char buf[96];
    int n = put_str(buf, 0, s);
    buf[n++] = '\n';
    sys(SYS_UART_WRITE, (long)buf, n);
}

static void spin(void) {
    for (volatile int i = 0; i < 400000; i++) { }
}

/* The buffer is on this process's own stack, which the fork left shared until
 * something writes to it -- so a mailbox call from the child is the sharpest
 * test that a user address reaching the kernel means the caller's page and not
 * some other process's. */
static void board_revision(const char *who) {
    unsigned int mbox[8] __attribute__((aligned(16)));

    mbox[0] = 8 * 4;                        /* buffer size */
    mbox[1] = 0;                            /* request */
    mbox[2] = MBOX_TAG_BOARD_REVISION;
    mbox[3] = 4;                            /* response size */
    mbox[4] = 0;                            /* request code */
    mbox[5] = 0;                            /* the answer lands here */
    mbox[6] = 0;                            /* end tag */
    mbox[7] = 0;

    if (sys(SYS_MBOX_CALL, MBOX_CH_PROP, (long)mbox) == 0) {
        say("user: mailbox call failed");
        return;
    }

    char buf[96];
    int n = put_str(buf, 0, who);
    n = put_str(buf, n, " read board revision ");
    n = put_hex(buf, n, mbox[5]);
    buf[n++] = '\n';
    sys(SYS_UART_WRITE, (long)buf, n);
}

__attribute__((section(".text.entry")))
void _start(void) {
    line("user: vm demo, pid=", sys(SYS_GETPID, 0, 0), "");

    /* Nothing is mapped until it is touched: the write below is what makes the
     * kernel find a frame for this page. */
    char *lazy = sys_mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS);
    line_hex("user: mmap anonymous -> ", (unsigned long)lazy);

    lazy[0] = 42;
    line("user: wrote 42, read back ", lazy[0], "");

    /* The kernel dereferences a user pointer directly rather than copying
     * through a bounce buffer, so it can be the one that meets a page nobody
     * has touched yet. That fault is taken with the kernel running, and has to
     * be repaired and resumed rather than being fatal. */
    char *untouched = sys_mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_ANONYMOUS);
    line_hex("user: handing the kernel an untouched page at ",
             (unsigned long)untouched);
    sys(SYS_UART_WRITE, (long)untouched, 1);
    say("user: kernel resumed the fault it took reading that page");

    /* MAP_POPULATE asks for the frames up front, so this one is already there. */
    char *eager = sys_mmap(0, 8192, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_POPULATE);
    eager[4096] = 7;
    line("user: populated region holds ", eager[4096], "");

    /* A page-aligned address nothing else is using is honoured as asked. */
    char *fixed = sys_mmap(0x20000000, 4096, PROT_READ | PROT_WRITE,
                           MAP_ANONYMOUS | MAP_POPULATE);
    line_hex("user: mmap at a chosen address -> ", (unsigned long)fixed);

    board_revision("user: parent");

    /* Both processes have this page mapped after the fork, and the first write
     * to it is what splits them apart. */
    char *shared = sys_mmap(0, 4096, PROT_READ | PROT_WRITE,
                            MAP_ANONYMOUS | MAP_POPULATE);
    shared[0] = 100;

    long child = sys(SYS_FORK, 0, 0);

    if (child == 0) {
        shared[0] = 200;
        line("user: child wrote 200, child sees ", shared[0], "");
        board_revision("user: child");
        sys(SYS_EXIT, 0, 0);
    }

    spin();
    line("user: parent still sees ", shared[0], " after the child wrote 200");

    /* A region that was never writable: this one really is an access
     * violation, and the kernel ends the process. */
    char *readonly = sys_mmap(0, 4096, PROT_READ, MAP_ANONYMOUS | MAP_POPULATE);
    say("user: writing to a read-only region, which should be fatal");
    readonly[0] = 1;

    say("user: THIS SHOULD NOT PRINT");
    sys(SYS_EXIT, 0, 0);
    for (;;) { }
}

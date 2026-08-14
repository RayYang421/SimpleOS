/* A user program for EL0, loaded from the initramfs by exec.
 *
 * It is linked at address 0 and loaded wherever the kernel finds room, so it
 * must not depend on its load address. GCC reaches its own strings with
 * adrp/add, which is PC-relative and therefore survives being moved; what
 * would break it is an absolute relocation, so there is no global data here
 * and nothing takes the address of a function.
 *
 * Every line is composed into a buffer and written with a single syscall. The
 * kernel holds off preemption for the length of one write, so a line stays
 * intact while still interleaving with the other process. */

#define SYS_GETPID      0
#define SYS_UART_WRITE  2
#define SYS_FORK        4
#define SYS_EXIT        5

static long sys(long n, long a0, long a1) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory", "cc");
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

static void line(const char *a, long v, const char *b) {
    char buf[96];
    int n = put_str(buf, 0, a);
    n = put_dec(buf, n, v);
    n = put_str(buf, n, b);
    buf[n++] = '\n';
    sys(SYS_UART_WRITE, (long)buf, n);
}

/* Long enough that the other process gets a turn in between, so the
 * interleaving in the output shows the timer really is preempting. */
static void spin(void) {
    for (volatile int i = 0; i < 400000; i++) { }
}

__attribute__((section(".text.entry")))
void _start(void) {
    line("user: running at EL0, pid=", sys(SYS_GETPID, 0, 0), "");

    long pid = sys(SYS_FORK, 0, 0);

    if (pid == 0) {
        line("user: child started, pid=", sys(SYS_GETPID, 0, 0), "");
        for (int i = 0; i < 3; i++) { line("user: child tick ", i, ""); spin(); }
        line("user: child exiting, pid=", sys(SYS_GETPID, 0, 0), "");
    } else {
        line("user: parent forked child pid=", pid, "");
        for (int i = 0; i < 3; i++) { line("user: parent tick ", i, ""); spin(); }
        line("user: parent exiting, pid=", sys(SYS_GETPID, 0, 0), "");
    }

    sys(SYS_EXIT, 0, 0);
    for (;;) { }
}

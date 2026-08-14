/* Signal demonstration. Registers a handler, sends itself a signal, and shows
 * that execution resumes where it left off once the handler returns -- the
 * kernel points the handler's return at a sigreturn trampoline, which restores
 * the context saved when the signal was delivered.
 *
 * Same relocatability rules as hello.c: no globals, nothing absolute. */

#define SYS_GETPID      0
#define SYS_UART_WRITE  2
#define SYS_FORK        4
#define SYS_EXIT        5
#define SYS_SIGNAL      8
#define SYS_SIGKILL     9

#define SIGUSR          5
#define SIGKILL         9

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

static void say(const char *s) {
    char buf[96];
    int n = put_str(buf, 0, s);
    buf[n++] = '\n';
    sys(SYS_UART_WRITE, (long)buf, n);
}

static void handler(void) {
    say("user: handler ran");
}

__attribute__((section(".text.entry")))
void _start(void) {
    long pid = sys(SYS_GETPID, 0, 0);
    line("user: sig demo, pid=", pid, "");

    sys(SYS_SIGNAL, SIGUSR, (long)handler);
    say("user: handler registered for signal 5");

    sys(SYS_SIGKILL, pid, SIGUSR);
    say("user: signal sent to self");

    /* Reached only if the handler returned through sigreturn and the original
     * context was put back. */
    say("user: resumed after the handler");

    /* No handler for SIGKILL, so the kernel's default action ends this. */
    say("user: sending SIGKILL to self");
    sys(SYS_SIGKILL, pid, SIGKILL);

    say("user: THIS SHOULD NOT PRINT");
    sys(SYS_EXIT, 0, 0);
    for (;;) { }
}

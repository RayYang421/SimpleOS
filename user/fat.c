/* FAT32 demonstration: reads a file the host put on the SD card, then writes
 * two of its own -- one followed by sync, one not.
 *
 * The order matters. Nothing written reaches the card until sync is called, so
 * after this program has run the card holds FAT_WS.TXT and not FAT_W.TXT: the
 * second file exists only in the cache, and would be gone if the board lost
 * power here.
 *
 * Same relocatability rules as hello.c -- no globals, nothing absolute. */

#define SYS_UART_WRITE  2
#define SYS_EXIT        5
#define SYS_OPEN        11
#define SYS_CLOSE       12
#define SYS_FWRITE      13
#define SYS_FREAD       14
#define SYS_SYNC        20

#define O_CREAT   00000100
#define STDOUT    1

static long sys3(long n, long a0, long a1, long a2) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc 0"
                     : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
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

static void emit(const char *buf, int n) {
    sys3(SYS_FWRITE, STDOUT, (long)buf, n);
}

static void say(const char *s) {
    char buf[160];
    int n = put_str(buf, 0, s);
    buf[n++] = '\n';
    emit(buf, n);
}

static void line(const char *a, long v, const char *b) {
    char buf[160];
    int n = put_str(buf, 0, a);
    n = put_dec(buf, n, v);
    n = put_str(buf, n, b);
    buf[n++] = '\n';
    emit(buf, n);
}

static void line_str(const char *a, const char *b) {
    char buf[160];
    int n = put_str(buf, 0, a);
    n = put_str(buf, n, b);
    buf[n++] = '\n';
    emit(buf, n);
}

/* Creates a file and writes one string to it. Returns what write reported. */
static long write_file(const char *path, const char *text) {
    long fd = sys3(SYS_OPEN, (long)path, O_CREAT, 0);
    if (fd < 0) return -1;

    int len = 0;
    while (text[len]) len++;

    long wrote = sys3(SYS_FWRITE, fd, (long)text, len);
    sys3(SYS_CLOSE, fd, 0, 0);
    return wrote;
}

__attribute__((section(".text.entry")))
void _start(void) {
    char buf[128];

    const char *banner = "user: fat demo starting\n";
    int n = 0;
    while (banner[n]) n++;
    sys3(SYS_UART_WRITE, (long)banner, n, 0);

    /* Read what the host put there. */
    long fd = sys3(SYS_OPEN, (long)"/boot/FAT_R.TXT", 0, 0);

    if (fd < 0) {
        say("user: no /boot/FAT_R.TXT -- is there a card?");
        sys3(SYS_EXIT, 0, 0, 0);
    }

    n = (int)sys3(SYS_FREAD, fd, (long)buf, 100);
    if (n < 0) n = 0;
    buf[n] = '\0';
    line("user: read ", n, " bytes from /boot/FAT_R.TXT");
    line_str("user: contents: ", buf);
    sys3(SYS_CLOSE, fd, 0, 0);

    /* Written, then synced: this one is on the card. */
    long wrote = write_file("/boot/FAT_WS.TXT", "written and synced\n");
    line("user: wrote ", wrote, " bytes to /boot/FAT_WS.TXT");

    long blocks = sys3(SYS_SYNC, 0, 0, 0);
    line("user: sync wrote ", blocks, " block(s) to the card");

    /* Written after the sync, and never synced: this one exists only in the
     * cache, and a power cut here would take it with it. */
    wrote = write_file("/boot/FAT_W.TXT", "written but never synced\n");
    line("user: wrote ", wrote, " bytes to /boot/FAT_W.TXT, without syncing");

    /* Both are readable from here, because reads come from the same cache the
     * writes went into. */
    fd = sys3(SYS_OPEN, (long)"/boot/FAT_W.TXT", 0, 0);
    n = (int)sys3(SYS_FREAD, fd, (long)buf, 100);
    if (n < 0) n = 0;
    buf[n] = '\0';
    line_str("user: reading it back anyway: ", buf);
    sys3(SYS_CLOSE, fd, 0, 0);

    say("user: fat demo done");
    sys3(SYS_EXIT, 0, 0, 0);
    for (;;) { }
}

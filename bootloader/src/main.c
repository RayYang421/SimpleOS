#include "uart.h"

/* Where the kernel is written and jumped to. The kernel is linked for an upper
 * half address, but this is the physical address that one maps back to -- it
 * builds its own page tables and moves there itself, so what arrives here is
 * still an image that starts running at 0x80000. Changing this means changing
 * KERNEL_PHYS_BASE in include/mmu.h and the kernel's linker script.
 *
 * The bootloader itself runs at 0x60000 (see linker.ld), so the two never
 * overlap and the receive loop cannot overwrite its own code. */
#define KERNEL_ADDR      0x80000UL

/* A kernel larger than this means the size field is garbage. */
#define MAX_KERNEL_SIZE  0x04000000UL   /* 64 MiB */

/* Once a transfer has started, a gap this long means the link died. There is
 * deliberately no timeout on the *initial* wait -- the bootloader should sit
 * at the prompt indefinitely until someone starts a transfer. */
#define RX_TIMEOUT_MS    10000UL

#define PROTO_MAGIC "OSCK"
#define ACK  0x06
#define NAK  0x15

static inline uint64_t read_cntfrq(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline uint64_t read_cntpct(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

/* Reads a byte, giving up after RX_TIMEOUT_MS. Returns 0 on success, -1 on
 * timeout. If the firmware left cntfrq_el0 at zero we have no usable clock, so
 * the wait becomes indefinite rather than expiring instantly. */
static int getc_timeout(uint8_t *out) {
    uint64_t freq = read_cntfrq();
    uint64_t deadline = 0;
    int have_clock = (freq != 0);

    if (have_clock) deadline = read_cntpct() + (freq / 1000UL) * RX_TIMEOUT_MS;

    for (;;) {
        if (uart_poll()) {
            *out = (uint8_t)uart_getc_raw();
            return 0;
        }
        if (have_clock && read_cntpct() >= deadline) return -1;
    }
}

static int read_u32_le(uint32_t *out) {
    uint32_t v = 0;

    for (int i = 0; i < 4; i++) {
        uint8_t b;
        if (getc_timeout(&b) != 0) return -1;
        v |= (uint32_t)b << (8 * i);
    }

    *out = v;
    return 0;
}

/* Blocks until the magic arrives, resynchronising on partial matches so that
 * leftover bytes on the line (a half-finished transfer, terminal noise) cannot
 * wedge us permanently out of step. */
static void wait_for_magic(void) {
    const char *magic = PROTO_MAGIC;
    int matched = 0;

    while (matched < 4) {
        uint8_t b = (uint8_t)uart_getc_raw();
        if (b == (uint8_t)magic[matched]) {
            matched++;
        } else {
            /* Restart, but treat this byte as a possible first byte so a run
             * like "OOSCK" still matches. */
            matched = (b == (uint8_t)magic[0]) ? 1 : 0;
        }
    }
}

static void fail(const char *msg) {
    uart_send(NAK);
    uart_puts("\nerror: ");
    uart_puts(msg);
    uart_puts("\n");
}

void main(uint64_t dtb_addr) {
    uart_init();

    uart_puts("\n== my-os UART bootloader ==\n");
    uart_puts("running at ");
    uart_hex((uint64_t)(uintptr_t)&main);
    uart_puts(", kernel will load at ");
    uart_hex(KERNEL_ADDR);
    uart_puts("\ndevicetree: ");
    uart_hex(dtb_addr);
    uart_puts("\nwaiting for kernel...\n");

    for (;;) {
        wait_for_magic();

        uint32_t size, want_sum;
        if (read_u32_le(&size) != 0 || read_u32_le(&want_sum) != 0) {
            fail("timed out reading header");
            continue;
        }

        if (size == 0 || size > MAX_KERNEL_SIZE) {
            fail("kernel size out of range");
            continue;
        }

        uart_send(ACK);                 /* header accepted, start streaming */

        uint8_t *dst = (uint8_t *)KERNEL_ADDR;
        uint32_t sum = 0;
        uint32_t i;

        for (i = 0; i < size; i++) {
            uint8_t b;
            if (getc_timeout(&b) != 0) break;
            dst[i] = b;
            sum += b;
        }

        if (i != size) {
            fail("transfer stalled");
            continue;
        }

        if (sum != want_sum) {
            fail("checksum mismatch");
            continue;
        }

        uart_send(ACK);                 /* image received intact */
        uart_puts("\nloaded ");
        uart_dec(size);
        uart_puts(" bytes, jumping to kernel...\n");

        /* Make the freshly written image visible to instruction fetch. Caches
         * are off this early, so this is belt and braces. */
        __asm__ volatile("dsb sy; ic iallu; isb" ::: "memory");

        /* Hand the devicetree pointer on in x0, exactly as the firmware
         * handed it to us -- the kernel reads the initramfs address out of it. */
        void (*entry)(uint64_t) = (void (*)(uint64_t))KERNEL_ADDR;
        entry(dtb_addr);

        for (;;) { }                    /* the kernel does not return */
    }
}

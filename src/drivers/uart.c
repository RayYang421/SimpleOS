#include "uart.h"
#include "gpio.h"
#include "mmio.h"
#include "irq.h"

/* Auxiliary / mini UART registers */
#define AUX_ENABLE      (MMIO_BASE + 0x00215004)
#define AUX_MU_IO       (MMIO_BASE + 0x00215040)
#define AUX_MU_IER      (MMIO_BASE + 0x00215044)
#define AUX_MU_IIR      (MMIO_BASE + 0x00215048)
#define AUX_MU_LCR      (MMIO_BASE + 0x0021504C)
#define AUX_MU_MCR      (MMIO_BASE + 0x00215050)
#define AUX_MU_LSR      (MMIO_BASE + 0x00215054)
#define AUX_MU_CNTL     (MMIO_BASE + 0x00215060)
#define AUX_MU_BAUD     (MMIO_BASE + 0x00215068)

#define LSR_RX_READY    0x01
#define LSR_TX_EMPTY    0x20

#define IER_RX          0x01
#define IER_TX          0x02

/* Power of two so the wrap is a mask rather than a division. */
#define BUF_SIZE        1024
#define BUF_MASK        (BUF_SIZE - 1)

static char rx_buf[BUF_SIZE];
static volatile uint32_t rx_head, rx_tail;

static char tx_buf[BUF_SIZE];
static volatile uint32_t tx_head, tx_tail;

static int async_enabled;

/* The mini UART's IER reads back bits the datasheet does not define, so the
 * enable state is tracked here instead of read-modify-writing the register. */
static uint32_t ier_shadow;

static uint64_t stat_rx, stat_tx, stat_irq;

static void (*preempt_hook)(int enable);

void uart_set_preempt_hook(void (*hook)(int enable)) { preempt_hook = hook; }

static inline void hold_preemption(void)    { if (preempt_hook) preempt_hook(0); }
static inline void release_preemption(void) { if (preempt_hook) preempt_hook(1); }

void uart_init(void) {
    /* --- GPIO: set pin14,15 to ALT5 (mini UART) --- */
    unsigned int r = mmio_read(GPFSEL1);
    r &= ~((7u << 12) | (7u << 15));   /* clear gpio14, gpio15 */
    r |=  ((2u << 12) | (2u << 15));   /* ALT5 = 010 */
    mmio_write(GPFSEL1, r);

    /* disable pull up/down for pin14,15 */
    mmio_write(GPPUD, 0);
    for (volatile int i = 0; i < 150; i++) { }
    mmio_write(GPPUDCLK0, (1u << 14) | (1u << 15));
    for (volatile int i = 0; i < 150; i++) { }
    mmio_write(GPPUDCLK0, 0);          /* flush */

    /* --- mini UART init (per hardware doc) --- */
    mmio_write(AUX_ENABLE, mmio_read(AUX_ENABLE) | 1); /* enable mini UART */
    mmio_write(AUX_MU_CNTL, 0);   /* disable TX/RX during config */
    ier_shadow = 0;
    mmio_write(AUX_MU_IER, 0);    /* no interrupt */
    mmio_write(AUX_MU_LCR, 3);    /* 8-bit mode */
    mmio_write(AUX_MU_MCR, 0);    /* no auto flow control */
    mmio_write(AUX_MU_BAUD, 270); /* 115200 baud @ 250MHz */
    mmio_write(AUX_MU_IIR, 6);    /* clear both FIFOs */
    mmio_write(AUX_MU_CNTL, 3);   /* enable TX/RX */

    async_enabled = 0;
}

/* --- polling primitives, used before interrupts are up and by the bootloader --- */

static void poll_send(char c) {
    while (!(mmio_read(AUX_MU_LSR) & LSR_TX_EMPTY)) { }
    mmio_write(AUX_MU_IO, c);
}

static char poll_recv(void) {
    while (!(mmio_read(AUX_MU_LSR) & LSR_RX_READY)) { }
    return mmio_read(AUX_MU_IO) & 0xFF;
}

char uart_getc_raw(void) {
    return poll_recv();
}

int uart_poll(void) {
    return (mmio_read(AUX_MU_LSR) & LSR_RX_READY) ? 1 : 0;
}

/* --- interrupt-driven mode --- */

void uart_enable_async(void) {
    uint64_t daif = irq_disable_save();

    rx_head = rx_tail = tx_head = tx_tail = 0;

    /* Receive interrupts on; transmit stays off until there is something to
     * send, otherwise the empty holding register interrupts continuously. */
    ier_shadow = IER_RX;
    mmio_write(AUX_MU_IER, ier_shadow);

    /* Let the AUX block's interrupt reach the core. */
    mmio_write(ENABLE_IRQS_1, IRQ_AUX_BIT);

    async_enabled = 1;

    /* Anything typed before now is still sitting in the device's FIFO, which
     * was the only buffer up to this point. Move it across rather than leaving
     * it to be overwritten. The FIFO is only eight bytes deep, so input that
     * arrived earlier still than that is already gone -- unavoidable on a line
     * with no flow control. */
    while (mmio_read(AUX_MU_LSR) & LSR_RX_READY) {
        char c = mmio_read(AUX_MU_IO) & 0xFF;
        uint32_t next = (rx_head + 1) & BUF_MASK;
        if (next == rx_tail) break;
        rx_buf[rx_head] = c;
        rx_head = next;
        stat_rx++;
    }

    irq_restore(daif);
}

/* Device half of the handler: move bytes between the hardware and the ring
 * buffers, then mask the AUX interrupt so the rest of the work can be done
 * outside interrupt context. irq.c re-enables it via uart_irq_complete. */
void uart_irq(void) {
    uint32_t iir = mmio_read(AUX_MU_IIR);

    stat_irq++;

    /* Bit 0 is clear while an interrupt is pending. */
    if (iir & 1) return;

    switch ((iir >> 1) & 3) {
    case 2:                                  /* receiver holds a valid byte */
        while (mmio_read(AUX_MU_LSR) & LSR_RX_READY) {
            char c = mmio_read(AUX_MU_IO) & 0xFF;
            uint32_t next = (rx_head + 1) & BUF_MASK;
            if (next != rx_tail) {           /* drop when the reader is behind */
                rx_buf[rx_head] = c;
                rx_head = next;
                stat_rx++;
            }
        }
        break;

    case 1:                                  /* transmit register is empty */
        while (tx_tail != tx_head && (mmio_read(AUX_MU_LSR) & LSR_TX_EMPTY)) {
            mmio_write(AUX_MU_IO, tx_buf[tx_tail]);
            tx_tail = (tx_tail + 1) & BUF_MASK;
            stat_tx++;
        }
        if (tx_tail == tx_head) {            /* nothing left to send */
            ier_shadow &= ~IER_TX;
            mmio_write(AUX_MU_IER, ier_shadow);
        }
        break;
    }

    mmio_write(DISABLE_IRQS_1, IRQ_AUX_BIT);
}

void uart_irq_complete(void) {
    mmio_write(ENABLE_IRQS_1, IRQ_AUX_BIT);
}

void uart_async_stats(uint64_t *rx, uint64_t *tx, uint64_t *interrupts) {
    if (rx) *rx = stat_rx;
    if (tx) *tx = stat_tx;
    if (interrupts) *interrupts = stat_irq;
}

/* --- the API the rest of the kernel uses --- */

void uart_send(char c) {
    if (!async_enabled) {
        poll_send(c);
        return;
    }

    uint64_t daif = irq_disable_save();
    uint32_t next = (tx_head + 1) & BUF_MASK;

    if (next == tx_tail) {
        /* Buffer full. Falling back to polling costs a stall but never
         * silently drops output. */
        irq_restore(daif);
        poll_send(c);
        return;
    }

    tx_buf[tx_head] = c;
    tx_head = next;

    ier_shadow |= IER_TX;                    /* ask to be told when it drains */
    mmio_write(AUX_MU_IER, ier_shadow);

    irq_restore(daif);
}

char uart_getc(void) {
    char c;

    if (!async_enabled) {
        c = poll_recv();
    } else {
        for (;;) {
            uint64_t daif = irq_disable_save();
            if (rx_head != rx_tail) {
                c = rx_buf[rx_tail];
                rx_tail = (rx_tail + 1) & BUF_MASK;
                irq_restore(daif);
                break;
            }
            irq_restore(daif);

            /* Nothing buffered: sleep until an interrupt arrives rather than
             * spinning on the device register. */
            __asm__ volatile("wfi");
        }
    }

    /* Terminals send CR for Enter; the shell reasons in terms of '\n'. */
    return c == '\r' ? '\n' : c;
}

void uart_flush(void) {
    if (!async_enabled) return;

    for (;;) {
        uint64_t daif = irq_disable_save();
        int empty = (tx_tail == tx_head);
        irq_restore(daif);
        if (empty) break;
        __asm__ volatile("wfi");
    }

    while (!(mmio_read(AUX_MU_LSR) & LSR_TX_EMPTY)) { }
}

void uart_puts(const char *s) {
    hold_preemption();
    while (*s) {
        if (*s == '\n') uart_send('\r'); /* terminals need CRLF */
        uart_send(*s++);
    }
    release_preemption();
}

void uart_write(const char *buf, size_t len) {
    hold_preemption();
    for (size_t i = 0; i < len; i++) {
        if (buf[i] == '\n') uart_send('\r');
        uart_send(buf[i]);
    }
    release_preemption();
}

void uart_hex(uint64_t v) {
    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned int d = (v >> shift) & 0xF;
        uart_send(d < 10 ? '0' + d : 'a' + d - 10);
    }
}

void uart_dec(uint64_t v) {
    char buf[21];
    int i = 0;

    if (v == 0) { uart_send('0'); return; }
    while (v > 0) { buf[i++] = '0' + (v % 10); v /= 10; }
    while (i > 0) uart_send(buf[--i]);
}

#include "uart.h"
#include "gpio.h"
#include "mmio.h"

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
    mmio_write(AUX_MU_IER, 0);    /* no interrupt */
    mmio_write(AUX_MU_LCR, 3);    /* 8-bit mode */
    mmio_write(AUX_MU_MCR, 0);    /* no auto flow control */
    mmio_write(AUX_MU_BAUD, 270); /* 115200 baud @ 250MHz */
    mmio_write(AUX_MU_IIR, 6);    /* no FIFO */
    mmio_write(AUX_MU_CNTL, 3);   /* enable TX/RX */
}

void uart_send(char c) {
    while (!(mmio_read(AUX_MU_LSR) & 0x20)) { } /* wait TX empty */
    mmio_write(AUX_MU_IO, c);
}

char uart_getc(void) {
    while (!(mmio_read(AUX_MU_LSR) & 0x01)) { } /* wait data ready */
    char c = mmio_read(AUX_MU_IO) & 0xFF;
    return c == '\r' ? '\n' : c;
}

void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n') uart_send('\r'); /* handle rn alignment */
        uart_send(*s++);
    }
}

char uart_getc_raw(void) {

    while (!(mmio_read(AUX_MU_LSR) & 0x01)) { }
    char c = mmio_read(AUX_MU_IO) & 0xFF;
    return c;
}
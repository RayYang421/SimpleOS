#ifndef UART_H
#define UART_H

#include "types.h"

void uart_init(void);
void uart_send(char c);
char uart_getc(void);
void uart_puts(const char *s);

/* Byte-exact input, for the bootloader's binary transfer: uart_getc translates
 * CR to LF for the shell, which would corrupt a kernel image. */
char uart_getc_raw(void);

/* Non-blocking: 1 if a byte is waiting, 0 otherwise. */
int uart_poll(void);

/* Writes exactly len bytes, including embedded NULs -- cat must not stop at
 * the first zero byte in a binary file. */
void uart_write(const char *buf, size_t len);

void uart_hex(uint64_t v);
void uart_dec(uint64_t v);

/* --- interrupt-driven mode ---
 *
 * Until this is called the driver busy-polls, which is what the bootloader
 * needs. Afterwards uart_send and uart_getc go through ring buffers and the
 * device is serviced from uart_irq, so the shell never blocks on the line. */
void uart_enable_async(void);

/* Two halves of the interrupt path: uart_irq does the device work and masks
 * the AUX interrupt, uart_irq_complete unmasks it once the queued task runs. */
void uart_irq(void);
void uart_irq_complete(void);

/* Busy-waits until every buffered byte has actually left the device. */
void uart_flush(void);

/* Bytes moved by the interrupt handler, for the shell's report. */
void uart_async_stats(uint64_t *rx, uint64_t *tx, uint64_t *interrupts);

#endif

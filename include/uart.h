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

#endif

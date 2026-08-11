#ifndef UART_H
#define UART_H

void uart_init(void);
void uart_send(char c);
char uart_getc(void);
void uart_puts(const char *s);
char uart_getc_raw(void);

#endif

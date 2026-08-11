#include "uart.h"
 
/* minimal strcmp: return 0 if equal */
static int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}
 
static void read_cmd(char *buf, int max) {
    int i = 0;
    while (i < max - 1) {
        char c = uart_getc();
        if (c == '\n') { uart_send('\r'); uart_send('\n'); break; }
        uart_send(c);          /* echo */
        buf[i++] = c;
    }
    buf[i] = '\0';
}
 
void shell(void) {
    char buf[128];
    uart_puts("Welcome to my-os shell!\n");
    while (1) {
        uart_puts("# ");
        read_cmd(buf, sizeof(buf));
        if (str_eq(buf, "help")) {
            uart_puts("help\t: print all available commands\n");
            uart_puts("hello\t: print Hello World!\n");
        } else if (str_eq(buf, "hello")) {
            uart_puts("Hello World!\n");
        } else if (buf[0] == '\0') {
            /* empty line, do nothing */
        } else {
            uart_puts("Unknown command: ");
            uart_puts(buf);
            uart_puts("\n");
        }
    }
}
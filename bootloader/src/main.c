#include "uart.h"

#define KERNEL_ADDR  0x80000


static unsigned int uart_read_uint(void) {
    
    unsigned int x = 0;
   
    for(volatile int i = 0; i < 4; i++){
        unsigned int tmp = uart_getc_raw();
        x |= tmp << (8 * i);
    }

    return x;
}

void main(void) {
    uart_init();
    uart_puts("== my-os UART bootloader ==\n");
    uart_puts("Send kernel image now...\n");

    unsigned int size = uart_read_uint();

    char *kernel = (char *)KERNEL_ADDR;

    for(volatile unsigned int i = 0; i < size; i++){
        kernel[i] = uart_getc_raw();
    }

    uart_puts("Loaded. Jumping to kernel...\n");

    void (*entry)(void) = (void (*)(void))KERNEL_ADDR;
    entry();

    while (1) { }
}
#include "uart.h"
 
void shell(void);
 
void main(void) {
    uart_init();
    shell();
}
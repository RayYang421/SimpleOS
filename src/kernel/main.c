#include "uart.h"
#include "fdt.h"
#include "cpio.h"
#include "shell.h"
#include "irq.h"
#include "timer.h"
#include "exception.h"

void main(uint64_t dtb_addr) {
    uart_init();

    /* Before anything is printed: the mini UART's FIFO is eight bytes deep and
     * nothing drains it until the RX interrupt is live, so input arriving
     * during boot would be lost. Within this block the order is forced -- the
     * vector table must exist before an interrupt can be taken, and the UART
     * only goes interrupt-driven once there is a handler for them. */
    irq_init();
    timer_init();
    uart_enable_async();
    irq_enable();

    uart_puts("\n== my-os kernel ==\n");

    fdt_init(dtb_addr);

    uart_puts("running at EL");
    uart_dec(current_el());
    uart_puts("   timer ");
    uart_dec(timer_freq() / 1000000);
    uart_puts(" MHz\n");

    uart_puts("devicetree: ");
    if (fdt_get_base()) uart_hex(fdt_get_base());
    else                uart_puts("none");

    uart_puts("   initramfs: ");
    uart_hex((uint64_t)(uintptr_t)cpio_get_base());
    if (!cpio_valid()) uart_puts(" (empty)");
    uart_puts("\n");

    shell();
}

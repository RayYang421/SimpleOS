#include "uart.h"
#include "fdt.h"
#include "cpio.h"
#include "shell.h"

/* dtb_addr is whatever start.S captured from x0 -- the devicetree pointer the
 * bootloader or firmware handed us. fdt_init validates it before trusting it,
 * and falls back to the platform's default initramfs address if there is no
 * usable devicetree. */
void main(uint64_t dtb_addr) {
    uart_init();

    uart_puts("\n== my-os kernel ==\n");

    fdt_init(dtb_addr);

    uart_puts("devicetree: ");
    if (fdt_get_base()) uart_hex(fdt_get_base());
    else                uart_puts("none");

    uart_puts("   initramfs: ");
    uart_hex((uint64_t)(uintptr_t)cpio_get_base());
    if (!cpio_valid()) uart_puts(" (empty)");
    uart_puts("\n");

    shell();
}

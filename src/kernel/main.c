#include "uart.h"
#include "fdt.h"
#include "cpio.h"
#include "shell.h"
#include "irq.h"
#include "timer.h"
#include "exception.h"
#include "mm.h"
#include "sched.h"
#include "vm.h"
#include "mmu.h"

/* The lab asks for the core timer frequency shifted right by five, which is a
 * slice of about 31 ms. */
#define SCHED_TICK_MS 31

static void shell_thread(void) {
    shell();
}

/* Rearms itself, so there is always a timer interrupt coming and therefore
 * always a preemption point, even when nothing else is scheduled. */
static void preempt_tick(void *arg) {
    (void)arg;
    timer_add_ms(preempt_tick, 0, SCHED_TICK_MS);
}

void main(uint64_t dtb_phys) {
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

    fdt_init(dtb_phys);

    uart_puts("running at EL");
    uart_dec(current_el());
    uart_puts("   timer ");
    uart_dec(timer_freq() / 1000000);
    uart_puts(" MHz   mmu on, kernel at ");
    uart_hex((uint64_t)(uintptr_t)main);
    uart_puts("\n");

    uart_puts("devicetree: ");
    if (fdt_get_base()) uart_hex(fdt_get_base());
    else                uart_puts("none");

    uart_puts("   initramfs: ");
    uart_hex(PA(cpio_get_base()));
    if (!cpio_valid()) uart_puts(" (empty)");
    uart_puts("\n");

    mem_init();

    /* The identity mapping the kernel booted through is only needed until it
     * is running at its linked address. Handing the lower half over to an
     * empty table turns a stray access to a low address into a fault, and
     * leaves ttbr0_el1 doing nothing but naming the current process. */
    mmu_drop_identity_map();

    /* Everything from here runs as a thread. The boot context becomes thread 0
     * and then the idle thread, so the run queue is never empty. */
    sched_init();
    timer_add_ms(preempt_tick, 0, SCHED_TICK_MS);
    thread_create(shell_thread);

    idle_thread();
}

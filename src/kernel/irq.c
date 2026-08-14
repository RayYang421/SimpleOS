#include "irq.h"
#include "timer.h"
#include "task.h"
#include "uart.h"
#include "sched.h"

void set_exception_vectors(void);   /* vectors.S */

void irq_init(void) {
    set_exception_vectors();
}

/* The UART's deferred half. Splitting it out this way keeps the driver free of
 * any dependency on the task queue, so the bootloader can link it too. */
static void uart_bottom_half(void *data) {
    (void)data;
    uart_irq_complete();
}

/* Reached from every IRQ vector entry. Two controllers have to be consulted:
 * the per-core QA7 register says whether this is the core timer or a
 * peripheral, and for a peripheral the BCM2837 pending register says which. */
void irq_entry(void) {
    uint32_t source = mmio_read(CORE0_IRQ_SOURCE);

    if (source & CORE_IRQ_TIMER_NS) {
        timer_irq();
    }

    if (source & CORE_IRQ_GPU) {
        if (mmio_read(IRQ_PENDING_1) & IRQ_AUX_BIT) {
            uart_irq();

            /* task_run_pending below always reaches this task before the
             * interrupt returns, so the mask window is short. If the queue is
             * full, unmask right away rather than wedging the UART. */
            if (task_add(uart_bottom_half, 0, TASK_PRIO_UART) != 0) {
                uart_irq_complete();
            }
        }
    }

    /* Deferred work runs here, with interrupts enabled. */
    task_run_pending();

    /* Preemption point: the current thread has had its slice, and its state is
     * safely on its own kernel stack, so switching here is transparent to
     * whatever was interrupted. */
    sched_tick();

    /* Mask again before returning: load_all and eret must not be interrupted
     * part-way through restoring the frame. eret puts back the interrupted
     * context's own mask from spsr_el1. */
    irq_disable();
}

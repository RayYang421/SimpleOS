#ifndef IRQ_H
#define IRQ_H

#include "types.h"
#include "mmio.h"

/* --- BCM2837 peripheral interrupt controller --- */
#define IRQ_BASIC_PENDING   (MMIO_BASE + 0x0000B200)
#define IRQ_PENDING_1       (MMIO_BASE + 0x0000B204)
#define IRQ_PENDING_2       (MMIO_BASE + 0x0000B208)
#define ENABLE_IRQS_1       (MMIO_BASE + 0x0000B210)
#define ENABLE_IRQS_2       (MMIO_BASE + 0x0000B214)
#define DISABLE_IRQS_1      (MMIO_BASE + 0x0000B21C)

/* The AUX block (which contains the mini UART) is IRQ 29 in bank 1. */
#define IRQ_AUX_BIT         (1u << 29)

/* --- QA7 per-core interrupt routing --- */
#define CORE0_TIMER_IRQ_CTRL (IO_BASE + 0x40000040UL)
#define CORE0_IRQ_SOURCE     (IO_BASE + 0x40000060UL)

#define CORE_IRQ_TIMER_NS    (1u << 1)   /* non-secure physical timer */
#define CORE_IRQ_GPU         (1u << 8)   /* peripheral IRQ routed to this core */

/* Masks IRQ/FIQ and returns the previous DAIF, for critical sections that a
 * handler must not run inside. Always pair with irq_restore. */
static inline uint64_t irq_disable_save(void) {
    uint64_t daif;
    __asm__ volatile("mrs %0, daif" : "=r"(daif));
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
    return daif;
}

static inline void irq_restore(uint64_t daif) {
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
}

static inline void irq_enable(void) {
    __asm__ volatile("msr daifclr, #0xf" ::: "memory");
}

static inline void irq_disable(void) {
    __asm__ volatile("msr daifset, #0xf" ::: "memory");
}

/* Installs the vector table and unmasks interrupts at the CPU. */
void irq_init(void);

/* Called from the vector table for every IRQ; works out the source and hands
 * off to the timer or UART handler. */
void irq_entry(void);

#endif

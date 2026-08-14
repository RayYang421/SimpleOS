#ifndef TIMER_H
#define TIMER_H

#include "types.h"

/* --- ARM generic timer, read straight from the CPU --- */
static inline uint64_t timer_count(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static inline uint64_t timer_freq(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

/* Enables the core timer and routes its interrupt to core 0. */
void timer_init(void);

/* Called from the IRQ dispatcher when the core timer fires. */
void timer_irq(void);

/* Toggles the lab's periodic "seconds since boot" report, which reprograms
 * itself every two seconds. Returns the new state. */
int timer_toggle_report(void);

/* --- Software timers multiplexed onto the single core timer ---
 *
 * The hardware gives one comparator, so timers are kept in a list sorted by
 * expiry and the comparator is always programmed for the earliest one.
 * Returns 0 on success, -1 if the timer pool is exhausted. */
int timer_add(void (*callback)(void *data), void *data, uint64_t after_seconds);

/* Sub-second granularity, which the scheduling tick needs. */
int timer_add_ms(void (*callback)(void *data), void *data, uint64_t after_ms);

/* Schedules a message to be printed after a delay, copying it into the timer
 * so the caller's buffer can be reused immediately. Backs the setTimeout
 * shell command. Returns 0 on success, -1 if the pool is exhausted. */
int timer_add_message(const char *msg, uint64_t after_seconds);

/* Timeouts registered but not yet fired. */
int timer_pending_count(void);

/* Seconds since boot, for messages. */
uint64_t timer_uptime_seconds(void);

/* Milliseconds since boot. Second resolution is too coarse to show that a
 * timeout landed when it was asked to. */
uint64_t timer_uptime_ms(void);

#endif

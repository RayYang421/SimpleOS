#ifndef EXCEPTION_H
#define EXCEPTION_H

#include "types.h"

/* Register state pushed by save_all in vectors.S. The layout is fixed by that
 * macro -- changing one without the other corrupts every exception.
 *
 *   x[0..30]   offsets 0..240
 *   elr        248   where the exception returned from
 *   spsr       256   processor state at the point of the exception
 *   sp_el0     264   the interrupted stack when coming from EL0
 */
struct trap_frame {
    uint64_t x[31];
    uint64_t elr;
    uint64_t spsr;
    uint64_t sp_el0;
};

void sync_handler(struct trap_frame *frame);
void sync_handler_el1(struct trap_frame *frame);
void invalid_handler(struct trap_frame *frame, uint64_t kind);

/* Drops to EL0 at entry with the given stack, and returns here when the user
 * program issues SYS_EXIT. Implemented in user.S. */
void enter_el0(void *entry, void *user_sp);

/* The demo user program: issues SVC five times, then SYS_EXIT. In user.S. */
void user_program(void);

/* Current exception level, for the boot banner. */
static inline uint64_t current_el(void) {
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return (el >> 2) & 3;
}

#endif

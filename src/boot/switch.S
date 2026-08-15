#include "syscall.h"

/* Context switch. Only the callee-saved registers need preserving: a switch
 * happens inside a function call, so everything else is either caller-saved or
 * already spilled by the compiler.
 *
 * The offsets here address struct cpu_context in include/sched.h, which is the
 * first member of struct thread -- so the thread pointer and its context share
 * an address. tpidr_el1 carries the current thread, which is how current()
 * works without a global. */

.section ".text"

/* void switch_to(struct thread *prev, struct thread *next) */
.global switch_to
switch_to:
    stp     x19, x20, [x0, #16 * 0]
    stp     x21, x22, [x0, #16 * 1]
    stp     x23, x24, [x0, #16 * 2]
    stp     x25, x26, [x0, #16 * 3]
    stp     x27, x28, [x0, #16 * 4]
    stp     x29, x30, [x0, #16 * 5]
    mov     x9, sp
    str     x9, [x0, #16 * 6]

    ldp     x19, x20, [x1, #16 * 0]
    ldp     x21, x22, [x1, #16 * 1]
    ldp     x23, x24, [x1, #16 * 2]
    ldp     x25, x26, [x1, #16 * 3]
    ldp     x27, x28, [x1, #16 * 4]
    ldp     x29, x30, [x1, #16 * 5]
    ldr     x9, [x1, #16 * 6]
    mov     sp, x9

    msr     tpidr_el1, x1
    ret

/* Installs the first thread without saving anything, for sched_init. */
.global set_current_thread
set_current_thread:
    msr     tpidr_el1, x0
    ret

/* Drops the calling kernel thread into user mode through the frame it is
 * given. The frame sits at the top of the thread's kernel stack, so load_all
 * leaves SP_EL1 back at the top, ready for the next exception from EL0. */
.global enter_user_mode
enter_user_mode:
    mov     sp, x0
    b       ret_to_user

/* A handler returns here, because signal_check sets lr to this address. It runs
 * at EL0, so it lives in the page every user address space maps -- the rest of
 * the kernel's text is not reachable from there. */
.section ".usertext", "ax"

.global sigreturn_trampoline
sigreturn_trampoline:
    mov     x8, #SYS_SIGRETURN
    svc     #0
1:
    b       1b                      /* sigreturn does not come back */

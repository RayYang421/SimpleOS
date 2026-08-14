/* Entering and leaving EL0. There is no process abstraction yet, so returning
 * from a user program means stashing the kernel's context on the way in and
 * restoring it when the program asks to exit.
 *
 * This is a setjmp/longjmp pair in all but name, so it has to save everything
 * the AAPCS says a call preserves. A user program is free to leave any
 * register in any state -- the demo one uses x19 as its counter -- and
 * leave_el0 returns straight into C rather than through eret, so nothing else
 * puts those registers back. */

#define RESUME_SP    0
#define RESUME_LR    8
#define RESUME_DAIF  16
#define RESUME_X29   24
#define RESUME_X19   32

.section ".text"

/* void enter_el0(void *entry, void *user_sp) */
.global enter_el0
enter_el0:
    adrp    x9, kernel_resume
    add     x9, x9, :lo12:kernel_resume

    mov     x10, sp
    /* Taking an exception masks DAIF, and leave_el0 bypasses the eret that
     * would normally put it back, so the mask is saved by hand too. */
    mrs     x11, daif

    stp     x10, x30, [x9, #RESUME_SP]
    stp     x11, x29, [x9, #RESUME_DAIF]
    stp     x19, x20, [x9, #RESUME_X19 + 0]
    stp     x21, x22, [x9, #RESUME_X19 + 16]
    stp     x23, x24, [x9, #RESUME_X19 + 32]
    stp     x25, x26, [x9, #RESUME_X19 + 48]
    stp     x27, x28, [x9, #RESUME_X19 + 64]

    msr     elr_el1, x0
    msr     sp_el0, x1

    /* EL0t with IRQ unmasked, so the core timer still fires while the user
     * program runs; D, A and F stay masked. */
    mov     x2, #0x340
    msr     spsr_el1, x2
    eret

/* Reached from the syscall handler for SYS_EXIT. Restoring the kernel stack
 * discards the exception frame along with it. */
.global leave_el0
leave_el0:
    adrp    x9, kernel_resume
    add     x9, x9, :lo12:kernel_resume

    ldp     x10, x30, [x9, #RESUME_SP]
    ldp     x11, x29, [x9, #RESUME_DAIF]
    ldp     x19, x20, [x9, #RESUME_X19 + 0]
    ldp     x21, x22, [x9, #RESUME_X19 + 16]
    ldp     x23, x24, [x9, #RESUME_X19 + 32]
    ldp     x25, x26, [x9, #RESUME_X19 + 48]
    ldp     x27, x28, [x9, #RESUME_X19 + 64]

    mov     sp, x10
    msr     daif, x11
    ret

/* save_all/load_all preserve every register across a trap, so the counter
 * survives in x19 with no effort here. */
.global user_program
user_program:
    mov     x19, #5
1:
    svc     #0
    sub     x19, x19, #1
    cbnz    x19, 1b

    svc     #1
2:
    b       2b

/* sp, lr, daif, x29, then x19-x28. */
.section ".bss"
.align 4
kernel_resume:
    .space 112

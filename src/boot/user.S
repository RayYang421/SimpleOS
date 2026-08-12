/* Entering and leaving EL0. There is no process abstraction yet, so returning
 * from a user program means stashing the kernel's stack pointer and link
 * register on the way in and restoring them when the program asks to exit. */

.section ".text"

.global enter_el0
enter_el0:
    adrp    x9, kernel_resume
    add     x9, x9, :lo12:kernel_resume
    mov     x10, sp
    /* Taking an exception masks DAIF, and leave_el0 returns to the kernel
     * directly rather than through eret, so the mask has to be restored by
     * hand or the kernel resumes with interrupts off for good. */
    mrs     x11, daif
    stp     x10, x30, [x9]
    str     x11, [x9, #16]

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
    ldp     x10, x30, [x9]
    ldr     x11, [x9, #16]
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

/* sp, lr, then the DAIF mask to restore. */
.section ".bss"
.align 4
kernel_resume:
    .space 32

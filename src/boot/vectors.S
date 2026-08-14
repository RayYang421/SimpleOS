/* EL1 exception vector table. The frame layout below must match struct
 * trap_frame in include/exception.h exactly. */

#define FRAME_SIZE   272
#define FRAME_ELR    248
#define FRAME_SPSR   256
#define FRAME_SP_EL0 264

/* elr_el1 and spsr_el1 go on the stack rather than staying in the system
 * registers: a nested interrupt would overwrite them and lose the outer
 * context. */
.macro save_all
    sub     sp, sp, #FRAME_SIZE
    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    str     x30,      [sp, #240]

    /* x9-x11 are on the stack now, so they are free as scratch. */
    mrs     x9,  elr_el1
    mrs     x10, spsr_el1
    mrs     x11, sp_el0
    stp     x9,  x10, [sp, #FRAME_ELR]
    str     x11,      [sp, #FRAME_SP_EL0]
.endm

/* The system registers are restored first, because the loads that follow
 * overwrite the scratch registers holding them. */
.macro load_all
    ldp     x9,  x10, [sp, #FRAME_ELR]
    ldr     x11,      [sp, #FRAME_SP_EL0]
    msr     elr_el1,  x9
    msr     spsr_el1, x10
    msr     sp_el0,   x11

    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    ldr     x30,      [sp, #240]
    add     sp, sp, #FRAME_SIZE
.endm

/* Every entry occupies exactly 0x80 bytes. */
.macro ventry label
.align 7
    b       \label
.endm

.macro invalid_entry kind
    save_all
    mov     x0, sp
    mov     x1, #\kind
    bl      invalid_handler
    b       .
.endm

.section ".text"

/* vbar_el1 requires the table to be 0x800-aligned. */
.align 11
.global exception_vector_table
exception_vector_table:
    ventry  el1t_sync
    ventry  el1t_irq
    ventry  el1t_fiq
    ventry  el1t_error

    ventry  el1h_sync
    ventry  el1h_irq
    ventry  el1h_fiq
    ventry  el1h_error

    ventry  el0_sync
    ventry  el0_irq
    ventry  el0_fiq
    ventry  el0_error

    ventry  el0_32_sync
    ventry  el0_32_irq
    ventry  el0_32_fiq
    ventry  el0_32_error

el1t_sync:      invalid_entry 0
el1t_irq:       invalid_entry 1
el1t_fiq:       invalid_entry 2
el1t_error:     invalid_entry 3

/* A synchronous fault inside the kernel is a bug, not something to resume. */
el1h_sync:      invalid_entry 4
el1h_fiq:       invalid_entry 6
el1h_error:     invalid_entry 7

el0_fiq:        invalid_entry 10
el0_error:      invalid_entry 11

el0_32_sync:    invalid_entry 12
el0_32_irq:     invalid_entry 13
el0_32_fiq:     invalid_entry 14
el0_32_error:   invalid_entry 15

el0_sync:
    save_all
    mov     x0, sp
    bl      sync_handler
    b       exit_to_user

el0_irq:
    save_all
    bl      irq_entry
    b       exit_to_user

/* An interrupt taken while already in the kernel returns straight to whatever
 * it interrupted; only a return to EL0 is a delivery point for signals. */
el1h_irq:
    save_all
    bl      irq_entry
    load_all
    eret

/* Last thing before dropping to EL0: a pending signal gets to redirect the
 * return by rewriting the frame that is about to be restored. */
exit_to_user:
    mov     x0, sp
    bl      signal_check
    load_all
    eret

/* Entry point for a thread that has never run in user mode, and for the child
 * of a fork: sp already points at the trap frame to return through. */
.global ret_to_user
ret_to_user:
    b       exit_to_user

.global set_exception_vectors
set_exception_vectors:
    adrp    x0, exception_vector_table
    add     x0, x0, :lo12:exception_vector_table
    msr     vbar_el1, x0
    isb
    ret

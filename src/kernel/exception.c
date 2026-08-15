#include "exception.h"
#include "uart.h"
#include "irq.h"
#include "syscall.h"
#include "sched.h"
#include "vm.h"

/* Exception Syndrome Register exception classes we name explicitly. */
#define EC_SVC_AARCH64       0x15
#define EC_INSN_ABORT_LOWER  0x20
#define EC_INSN_ABORT_SAME   0x21
#define EC_DATA_ABORT_LOWER  0x24
#define EC_DATA_ABORT_SAME   0x25

static uint64_t read_esr(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(v));
    return v;
}

static uint64_t read_far(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, far_el1" : "=r"(v));
    return v;
}

/* Read from the saved frame rather than the live system registers: a nested
 * exception would have replaced those. */
static void report(const char *what, struct trap_frame *frame, uint64_t esr) {
    uart_puts(what);
    uart_puts("\n  spsr_el1: "); uart_hex(frame->spsr);
    uart_puts("\n  elr_el1 : "); uart_hex(frame->elr);
    uart_puts("\n  esr_el1 : "); uart_hex(esr);
    uart_puts("\n");
}

void sync_handler(struct trap_frame *frame) {
    uint64_t esr = read_esr();
    uint64_t ec  = esr >> 26;

    if (ec == EC_SVC_AARCH64) {
        /* The call is selected by x8, per the lab's convention; the SVC
         * immediate is ignored. Arguments arrive in x0.. and the result goes
         * back in x0, both through the saved frame. */
        syscall_dispatch(frame);
        return;
    }

    /* Most of what is left is a page the process is entitled to but has never
     * touched. Anything the address space cannot explain is fatal. */
    if (vm_fault(esr, read_far()) == 0) return;

    invalid_handler(frame, 99);

    struct thread *t = current();
    if (t && t->pgd) thread_exit(-1);    /* a user process, so end it */
    for (;;) { }
}

/* Faults taken with the kernel already running. A syscall that was handed a
 * user pointer dereferences it directly, so it can meet a page that has not
 * been faulted in yet, or one that a fork left shared -- both are the address
 * space's business and resumable. Everything else is a kernel bug. */
void sync_handler_el1(struct trap_frame *frame) {
    if (vm_fault(read_esr(), read_far()) == 0) return;

    invalid_handler(frame, 4);
    for (;;) { }
}

/* No recovery path: this prints what it can, and vectors.S parks the core. */
void invalid_handler(struct trap_frame *frame, uint64_t kind) {
    uint64_t esr = read_esr();
    uint64_t ec  = esr >> 26;

    uart_puts("\n*** unhandled exception (vector entry ");
    uart_dec(kind);
    uart_puts(") ***\n");
    report("  state:", frame, esr);

    if (ec == EC_DATA_ABORT_LOWER || ec == EC_DATA_ABORT_SAME ||
        ec == EC_INSN_ABORT_LOWER || ec == EC_INSN_ABORT_SAME) {
        uart_puts("  far_el1 : ");
        uart_hex(read_far());
        uart_puts("   (faulting address)\n");
    }

    uart_puts("  system halted\n");
}

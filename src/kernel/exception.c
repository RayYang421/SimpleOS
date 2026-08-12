#include "exception.h"
#include "uart.h"
#include "irq.h"

void leave_el0(void);   /* user.S */

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
        /* For an SVC the syndrome's low 16 bits are the instruction's
         * immediate, which is what selects the call. */
        uint32_t number = (uint32_t)(esr & 0xFFFF);

        switch (number) {
        case SYS_PRINT_EXC:
            report("[svc] exception taken from EL0", frame, esr);
            return;

        case SYS_EXIT:
            uart_puts("[svc] user program exited, back to the kernel\n");
            leave_el0();            /* discards this frame; does not return */
            return;

        default:
            uart_puts("[svc] unknown syscall number ");
            uart_dec(number);
            uart_puts("\n");
            return;
        }
    }

    invalid_handler(frame, 99);
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

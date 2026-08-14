#include "signal.h"
#include "sched.h"
#include "syscall.h"
#include "mm.h"
#include "uart.h"
#include "irq.h"
#include "string.h"

#define SIG_STACK_SIZE 0x2000

void sigreturn_trampoline(void);    /* switch.S */

void signal_register(int sig, void (*handler)(void)) {
    if (sig <= 0 || sig >= MAX_SIGNALS) return;
    current()->sig_handler[sig] = handler;
}

void signal_send(int pid, int sig) {
    if (sig <= 0 || sig >= MAX_SIGNALS) return;

    struct thread *t = thread_by_pid(pid);
    if (t == 0 || t->state == THREAD_ZOMBIE) return;

    uint64_t daif = irq_disable_save();
    t->sig_pending |= (1u << sig);
    irq_restore(daif);
}

/* Runs just before the return to EL0, with the frame that is about to be
 * restored. Rewriting it here is what diverts the thread into its handler. */
void signal_check(struct trap_frame *tf) {
    struct thread *t = current();

    if (t == 0 || t->sig_pending == 0) return;

    /* Nested handlers are out of scope: the saved frame would be overwritten
     * and the original context lost. */
    if (t->in_signal) return;

    uint64_t daif = irq_disable_save();
    int sig = 0;
    for (int i = 1; i < MAX_SIGNALS; i++) {
        if (t->sig_pending & (1u << i)) { sig = i; break; }
    }
    if (sig) t->sig_pending &= ~(1u << sig);
    irq_restore(daif);

    if (sig == 0) return;

    void (*handler)(void) = t->sig_handler[sig];

    if (handler == 0) {
        /* The only default action implemented is SIGKILL's; everything else
         * is ignored, which is what an unhandled signal does here. */
        if (sig == SIGKILL) {
            uart_puts("\n[signal] pid ");
            uart_dec(t->pid);
            uart_puts(" killed by SIGKILL\n");
            thread_exit(-1);
        }
        return;
    }

    if (t->sig_stack == 0) {
        t->sig_stack = kmalloc(SIG_STACK_SIZE);
        if (t->sig_stack == 0) return;
    }

    /* Stash the interrupted context so sigreturn can put it back, then point
     * the frame at the handler. The handler returns to a trampoline that
     * issues sigreturn, which is how control comes back here. */
    t->sig_saved = *tf;
    t->in_signal = 1;

    tf->elr    = (uint64_t)(uintptr_t)handler;
    tf->sp_el0 = (uint64_t)(uintptr_t)t->sig_stack + SIG_STACK_SIZE;
    tf->x[30]  = (uint64_t)(uintptr_t)sigreturn_trampoline;
    tf->x[0]   = (uint64_t)sig;
}

void signal_return(struct trap_frame *tf) {
    struct thread *t = current();

    if (!t->in_signal) return;

    *tf = t->sig_saved;
    t->in_signal = 0;
}

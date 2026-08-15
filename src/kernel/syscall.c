#include "syscall.h"
#include "sched.h"
#include "signal.h"
#include "vm.h"
#include "mmu.h"
#include "mbox.h"
#include "uart.h"
#include "cpio.h"
#include "string.h"
#include "irq.h"

void leave_el0(void);   /* user.S, for the lab 3 demo */

/* SPSR for EL0t with IRQ unmasked, so a user program stays preemptible. */
#define USER_SPSR 0x340

static void report_exception(struct trap_frame *tf) {
    uint64_t esr;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(esr));

    uart_puts("[svc] exception taken from EL0");
    uart_puts("\n  spsr_el1: "); uart_hex(tf->spsr);
    uart_puts("\n  elr_el1 : "); uart_hex(tf->elr);
    uart_puts("\n  esr_el1 : "); uart_hex(esr);
    uart_puts("\n");
}

/* Every process is loaded at address 0 of an address space of its own, so a
 * program no longer has to be built to run from wherever there happened to be
 * room -- and two of them can be running at the same address at once. */
int do_exec(struct trap_frame *tf, const char *name) {
    const char *data;
    size_t size;

    if (cpio_lookup(name, &data, &size) != 0 || size == 0) return -1;

    struct thread *t = current();

    preempt_disable();

    if (vm_new_address_space(t, data, size) != 0) {
        preempt_enable();
        return -1;
    }

    /* Rewriting the frame the syscall will return through is what makes the
     * return land in the new program rather than back in the caller. Nothing
     * is mapped yet: the first instruction fetch faults, and the fault handler
     * copies that page of the image in. */
    memset(tf, 0, sizeof(*tf));
    tf->elr    = USER_TEXT_VA;
    tf->sp_el0 = USER_STACK_TOP;
    tf->spsr   = USER_SPSR;

    vm_switch(t->pgd);
    preempt_enable();
    return 0;
}

/* The child gets a copy of the parent's address space -- the regions outright,
 * the pages themselves only as far as marking both sides read-only, so a frame
 * is copied when one of them writes to it and not before. */
static int do_fork(struct trap_frame *tf) {
    struct thread *parent = current();

    if (parent->pgd == 0) return -1;      /* not a user process */

    struct thread *child = thread_alloc();
    if (child == 0) return -1;

    preempt_disable();

    if (vm_fork(child, parent) != 0) {
        /* Half-built and never queued, so the idle thread is the only one that
         * can clean it up. */
        child->state = THREAD_ZOMBIE;
        preempt_enable();
        return -1;
    }

    /* Returning from the same syscall the parent is in, just with a different
     * answer. Every address in the frame means the same thing in the child's
     * address space as it does in the parent's, so nothing has to be rebased. */
    *child->tf = *tf;
    child->tf->x[0] = 0;                  /* the child's fork() returns 0 */

    for (int i = 0; i < MAX_SIGNALS; i++)
        child->sig_handler[i] = parent->sig_handler[i];

    /* Scheduled straight back into user mode through its own frame. */
    child->ctx.sp = (uint64_t)(uintptr_t)child->tf;
    child->ctx.lr = (uint64_t)(uintptr_t)ret_to_user;
    sched_enqueue(child);

    preempt_enable();
    return child->pid;                    /* the parent's fork() returns this */
}

static size_t do_uart_read(char *buf, size_t size) {
    for (size_t i = 0; i < size; i++) buf[i] = uart_getc();
    return size;
}

static size_t do_uart_write(const char *buf, size_t size) {
    uart_write(buf, size);
    return size;
}

/* The VideoCore is given a physical address and knows nothing about page
 * tables, so the message is relayed through a buffer the kernel owns. That also
 * settles what a mailbox buffer means under copy-on-write: it is read out of
 * the caller's address space and written back into it, both through the
 * caller's own mapping, so a forked child's buffer is its own. */
static int do_mbox_call(unsigned char channel, unsigned int *user_mbox) {
    static volatile uint32_t relay[36] __attribute__((aligned(16)));

    if (user_mbox == 0) return 0;

    uint64_t daif = irq_disable_save();

    uint32_t size = user_mbox[0] / 4;
    if (size == 0 || size > 36) { irq_restore(daif); return 0; }

    for (uint32_t i = 0; i < size; i++) relay[i] = user_mbox[i];

    int ok = mbox_call(channel, (unsigned int *)relay);

    for (uint32_t i = 0; i < size; i++) user_mbox[i] = relay[i];

    irq_restore(daif);
    return ok;
}

void syscall_dispatch(struct trap_frame *tf) {
    uint64_t num = tf->x[8];

    switch (num) {
    case SYS_GETPID:
        tf->x[0] = (uint64_t)current()->pid;
        break;

    case SYS_UART_READ:
        tf->x[0] = do_uart_read((char *)(uintptr_t)tf->x[0], (size_t)tf->x[1]);
        break;

    case SYS_UART_WRITE:
        tf->x[0] = do_uart_write((const char *)(uintptr_t)tf->x[0], (size_t)tf->x[1]);
        break;

    case SYS_EXEC:
        tf->x[0] = (uint64_t)(int64_t)do_exec(tf, (const char *)(uintptr_t)tf->x[0]);
        break;

    case SYS_FORK:
        tf->x[0] = (uint64_t)(int64_t)do_fork(tf);
        break;

    case SYS_EXIT:
        thread_exit((int)tf->x[0]);
        break;

    case SYS_MBOX_CALL:
        tf->x[0] = (uint64_t)do_mbox_call((unsigned char)tf->x[0],
                                          (unsigned int *)(uintptr_t)tf->x[1]);
        break;

    case SYS_KILL:
        signal_send((int)tf->x[0], SIGKILL);
        break;

    case SYS_SIGNAL:
        signal_register((int)tf->x[0], (void (*)(void))(uintptr_t)tf->x[1]);
        break;

    case SYS_SIGKILL:
        signal_send((int)tf->x[0], (int)tf->x[1]);
        break;

    case SYS_MMAP:
        tf->x[0] = (uint64_t)(uintptr_t)vm_mmap(tf->x[0], tf->x[1],
                                                (int)tf->x[2], (int)tf->x[3]);
        break;

    case SYS_SIGRETURN:
        signal_return(tf);
        break;

    /* --- the lab 3 exception demo, which predates this numbering --- */
    case SYS_DEMO_REPORT:
        report_exception(tf);
        break;

    case SYS_DEMO_LEAVE:
        uart_puts("[svc] user program exited, back to the kernel\n");
        leave_el0();                      /* discards this frame */
        break;

    default:
        uart_puts("[svc] unknown syscall number ");
        uart_dec(num);
        uart_puts("\n");
        tf->x[0] = (uint64_t)-1;
        break;
    }
}

#include "syscall.h"
#include "sched.h"
#include "signal.h"
#include "mbox.h"
#include "uart.h"
#include "cpio.h"
#include "mm.h"
#include "page.h"
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

int do_exec(struct trap_frame *tf, const char *name) {
    const char *data;
    size_t size;

    if (cpio_lookup(name, &data, &size) != 0 || size == 0) return -1;

    struct thread *t = current();

    /* The image must land on a page boundary. A position-independent binary
     * reaches its own data with adrp, which is *page* relative -- it rounds
     * the PC down to 4 KiB and adds the link-time offset. Linked at 0 and
     * loaded page-aligned that works out; loaded at any other offset, every
     * data reference is wrong by the difference. kmalloc would hand back a
     * chunk part-way into a page, so take whole frames instead. */
    int order = 0;
    while ((PAGE_SIZE << order) < size) order++;

    void *image = page_alloc(order);
    if (image == 0) return -1;
    memcpy(image, data, size);

    void *stack = kmalloc(USTACK_SIZE);
    if (stack == 0) {
        kfree(image);
        return -1;
    }

    preempt_disable();

    if (t->prog)   kfree(t->prog);
    if (t->ustack) kfree(t->ustack);

    t->prog        = image;
    t->prog_size   = size;
    t->ustack      = stack;
    t->ustack_size = USTACK_SIZE;

    /* Rewriting the frame the syscall will return through is what makes the
     * return land in the new program rather than back in the caller. */
    memset(tf, 0, sizeof(*tf));
    tf->elr    = (uint64_t)(uintptr_t)image;
    tf->sp_el0 = (uint64_t)(uintptr_t)stack + USTACK_SIZE;
    tf->spsr   = USER_SPSR;

    preempt_enable();
    return 0;
}

/* The child gets its own copy of the user stack and of the trap frame, so it
 * returns from the same syscall the parent is in -- just with a different
 * answer. The program image itself is shared. */
static int do_fork(struct trap_frame *tf) {
    struct thread *parent = current();

    if (parent->ustack == 0) return -1;   /* nothing to duplicate */

    struct thread *child = thread_alloc();
    if (child == 0) return -1;

    child->ustack = kmalloc(parent->ustack_size);
    if (child->ustack == 0) return -1;

    child->ustack_size = parent->ustack_size;
    memcpy(child->ustack, parent->ustack, parent->ustack_size);

    /* Shared image: exec would replace it, and kill_zombies must not free it
     * twice, so only the parent owns it. */
    child->prog      = 0;
    child->prog_size = 0;

    *child->tf = *tf;

    /* Anything that pointed into the parent's stack has to be rebased onto the
     * copy, at the same offset. */
    uint64_t base = (uint64_t)(uintptr_t)parent->ustack;
    uint64_t top  = base + parent->ustack_size;
    uint64_t cbase = (uint64_t)(uintptr_t)child->ustack;

    if (tf->sp_el0 >= base && tf->sp_el0 <= top)
        child->tf->sp_el0 = cbase + (tf->sp_el0 - base);
    if (tf->x[29] >= base && tf->x[29] <= top)
        child->tf->x[29] = cbase + (tf->x[29] - base);

    child->tf->x[0] = 0;                  /* the child's fork() returns 0 */

    for (int i = 0; i < MAX_SIGNALS; i++)
        child->sig_handler[i] = parent->sig_handler[i];

    /* Scheduled straight back into user mode through its own frame. */
    child->ctx.sp = (uint64_t)(uintptr_t)child->tf;
    child->ctx.lr = (uint64_t)(uintptr_t)ret_to_user;
    sched_enqueue(child);

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
        tf->x[0] = (uint64_t)mbox_call((unsigned char)tf->x[0],
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

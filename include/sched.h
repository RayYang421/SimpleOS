#ifndef SCHED_H
#define SCHED_H

#include "types.h"
#include "exception.h"

#define KSTACK_SIZE 0x4000      /* 16 KiB */
#define USTACK_SIZE 0x4000

#define MAX_SIGNALS 16
#define SIGKILL     9

enum thread_state {
    THREAD_UNUSED = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_ZOMBIE,          /* finished, waiting for the idle thread to reap it */
};

/* Callee-saved state, which is all a cooperative switch has to preserve --
 * everything else is either caller-saved or already on the stack. switch_to
 * addresses these by offset, so the layout is fixed and this must stay the
 * first member of struct thread. */
struct cpu_context {
    uint64_t x19, x20, x21, x22, x23, x24, x25, x26, x27, x28;
    uint64_t fp;            /* x29 */
    uint64_t lr;            /* x30 */
    uint64_t sp;
};

struct thread {
    struct cpu_context ctx;         /* offset 0, see above */

    int pid;
    int state;
    int exit_code;

    void (*entry)(void);            /* kernel threads only */

    void *kstack;                   /* allocation base, not the top */
    void *ustack;
    uint64_t ustack_size;

    /* The loaded user image. Shared with a forked child -- only the stack is
     * duplicated, so a program that keeps state in its own data section would
     * see it shared. */
    void *prog;
    uint64_t prog_size;

    /* Where the thread's user-mode register state lives while it is in the
     * kernel: the top of its own kernel stack. */
    struct trap_frame *tf;

    /* Signal handlers, and the frame stashed while one is running. */
    void (*sig_handler[MAX_SIGNALS])(void);
    uint32_t sig_pending;
    int in_signal;
    struct trap_frame sig_saved;
    void *sig_stack;

    struct thread *next;            /* run queue */
    struct thread *all_next;        /* every live thread, for kill() */
};

/* Turns the boot context into thread 0 so there is always a current thread. */
void sched_init(void);

struct thread *thread_create(void (*entry)(void));

/* Allocates a thread and its kernel stack without queueing it or giving it an
 * entry point, for fork to fill in. */
struct thread *thread_alloc(void);
void sched_enqueue(struct thread *t);
void thread_exit(int code);
void schedule(void);
void kill_zombies(void);

/* Never returns: reaps zombies and yields, which is what keeps the run queue
 * from ever being empty. */
void idle_thread(void);

struct thread *thread_by_pid(int pid);
void thread_list(void);

static inline struct thread *current(void) {
    uint64_t t;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(t));
    return (struct thread *)t;
}

/* Raised around critical sections so a timer interrupt cannot switch away
 * mid-update; schedule() becomes a no-op while it is non-zero. */
void preempt_disable(void);
void preempt_enable(void);
int  preempt_active(void);

/* Called from the timer interrupt. Switches only when it is safe to. */
void sched_tick(void);

/* Drops the current thread into EL0 through its trap frame. In switch.S. */
void ret_to_user(void);
void enter_user_mode(struct trap_frame *tf);

#endif

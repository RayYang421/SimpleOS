#include "sched.h"
#include "mm.h"
#include "uart.h"
#include "irq.h"
#include "string.h"

void switch_to(struct thread *prev, struct thread *next);   /* switch.S */
void set_current_thread(struct thread *t);                  /* switch.S */

static struct thread *rq_head, *rq_tail;    /* ready threads, FIFO */
static struct thread *all_threads;          /* every live thread */
static int next_pid = 1;
static int preempt_count;

static void rq_push(struct thread *t) {
    t->next = 0;
    if (rq_tail) rq_tail->next = t;
    else         rq_head = t;
    rq_tail = t;
}

static struct thread *rq_pop(void) {
    struct thread *t = rq_head;
    if (t) {
        rq_head = t->next;
        if (rq_head == 0) rq_tail = 0;
        t->next = 0;
    }
    return t;
}

void preempt_disable(void) { preempt_count++; }
void preempt_enable(void)  { if (preempt_count > 0) preempt_count--; }
int  preempt_active(void)  { return preempt_count == 0; }

/* Where a newly created kernel thread starts. switch_to returns straight here
 * the first time the thread is scheduled, so it has to enable interrupts
 * itself -- it never passes through the tail of schedule() that would
 * otherwise restore them. */
static void thread_start(void) {
    struct thread *t = current();

    irq_enable();
    if (t->entry) t->entry();
    thread_exit(0);
}

/* Handed to the UART driver so a message cannot be split across a context
 * switch. */
static void uart_preempt_hook(int enable) {
    if (enable) preempt_enable();
    else        preempt_disable();
}

void sched_init(void) {
    struct thread *t = kmalloc(sizeof(struct thread));
    if (t == 0) {
        uart_puts("sched_init: out of memory\n");
        return;
    }

    memset(t, 0, sizeof(*t));
    t->pid   = 0;
    t->state = THREAD_RUNNING;
    /* Thread 0 runs on the boot stack, which the linker script already
     * reserves, so kstack stays null and it is never freed. */

    all_threads = t;
    set_current_thread(t);

    uart_set_preempt_hook(uart_preempt_hook);
}

struct thread *thread_alloc(void) {
    struct thread *t = kmalloc(sizeof(struct thread));
    if (t == 0) return 0;

    memset(t, 0, sizeof(*t));

    t->kstack = kmalloc(KSTACK_SIZE);
    if (t->kstack == 0) {
        kfree(t);
        return 0;
    }

    uint64_t daif = irq_disable_save();

    t->pid   = next_pid++;
    t->state = THREAD_UNUSED;

    /* The trap frame is reserved at the very top of the kernel stack, so an
     * exception taken from EL0 -- which pushes at whatever SP_EL1 holds --
     * lands exactly there, and load_all leaves SP_EL1 back at the top again.
     *
     * The thread's own kernel stack therefore has to start *below* the frame,
     * not at the top of the allocation: starting at the top would let its
     * ordinary call frames grow straight through the reserved trap frame. */
    uint64_t ktop = (uint64_t)(uintptr_t)t->kstack + KSTACK_SIZE;
    t->tf = (struct trap_frame *)(ktop - sizeof(struct trap_frame));

    t->ctx.sp = (uint64_t)(uintptr_t)t->tf;

    t->all_next = all_threads;
    all_threads = t;

    irq_restore(daif);
    return t;
}

void sched_enqueue(struct thread *t) {
    uint64_t daif = irq_disable_save();
    t->state = THREAD_READY;
    rq_push(t);
    irq_restore(daif);
}

struct thread *thread_create(void (*entry)(void)) {
    struct thread *t = thread_alloc();
    if (t == 0) return 0;

    t->entry  = entry;
    t->ctx.lr = (uint64_t)(uintptr_t)thread_start;
    sched_enqueue(t);
    return t;
}

void schedule(void) {
    uint64_t daif = irq_disable_save();

    struct thread *prev = current();
    if (prev == 0) {                /* scheduler not started yet */
        irq_restore(daif);
        return;
    }

    struct thread *next = rq_pop();

    if (next == 0) {
        /* Nobody else is ready. A zombie with no successor would be a dead
         * end, so keep running rather than switching to nothing. */
        irq_restore(daif);
        return;
    }

    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_READY;
        rq_push(prev);
    }

    next->state = THREAD_RUNNING;
    switch_to(prev, next);

    /* Reached only when something schedules this thread again; daif is on this
     * thread's own stack, so it is still the value this thread saved. */
    irq_restore(daif);
}

/* Interrupts arrive from the moment the UART goes asynchronous, which is well
 * before sched_init runs, so this has to tolerate there being no current
 * thread yet. */
void sched_tick(void) {
    if (current() == 0) return;
    if (preempt_count == 0) schedule();
}

void thread_exit(int code) {
    struct thread *t = current();

    uint64_t daif = irq_disable_save();
    t->exit_code = code;
    t->state = THREAD_ZOMBIE;
    irq_restore(daif);

    schedule();

    /* schedule() only returns if nothing else was ready; there is nothing left
     * to do but offer the CPU again until the idle thread reaps this. */
    for (;;) schedule();
}

void kill_zombies(void) {
    uint64_t daif = irq_disable_save();

    struct thread **link = &all_threads;
    while (*link) {
        struct thread *t = *link;

        if (t->state == THREAD_ZOMBIE && t != current()) {
            *link = t->all_next;
            irq_restore(daif);

            /* Freed outside the critical section: kfree can return a frame to
             * the buddy system, which is more work than belongs with
             * interrupts off. */
            if (t->kstack)    kfree(t->kstack);
            if (t->ustack)    kfree(t->ustack);
            if (t->prog)      kfree(t->prog);
            if (t->sig_stack) kfree(t->sig_stack);
            kfree(t);

            daif = irq_disable_save();
            link = &all_threads;        /* the list moved under us */
            continue;
        }

        link = &t->all_next;
    }

    irq_restore(daif);
}

void idle_thread(void) {
    for (;;) {
        kill_zombies();

        uint64_t daif = irq_disable_save();
        int nothing_ready = (rq_head == 0);
        irq_restore(daif);

        /* Sleeping until the next interrupt keeps the emulator (and a real
         * board) from spinning; the timer tick guarantees a wake-up. */
        if (nothing_ready) __asm__ volatile("wfi");

        schedule();
    }
}

struct thread *thread_by_pid(int pid) {
    uint64_t daif = irq_disable_save();
    struct thread *found = 0;

    for (struct thread *t = all_threads; t; t = t->all_next) {
        if (t->pid == pid) { found = t; break; }
    }

    irq_restore(daif);
    return found;
}

void thread_list(void) {
    static const char *state_name[] = { "unused", "ready", "running", "zombie" };

    uart_puts("pid\tstate\tkstack\n");

    uint64_t daif = irq_disable_save();
    for (struct thread *t = all_threads; t; t = t->all_next) {
        uart_dec(t->pid);
        uart_puts("\t");
        uart_puts(t->state <= THREAD_ZOMBIE ? state_name[t->state] : "?");
        uart_puts("\t");
        uart_hex((uint64_t)(uintptr_t)t->kstack);
        if (t == current()) uart_puts("  <- current");
        uart_puts("\n");
    }
    irq_restore(daif);
}

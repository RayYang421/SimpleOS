#include "task.h"
#include "irq.h"

#define MAX_TASKS 64

/* Higher than any real priority, so an idle system runs everything. */
#define PRIO_NONE (TASK_PRIO_IDLE + 100)

struct task {
    void (*callback)(void *data);
    void *data;
    int priority;
    int in_use;
    struct task *next;
};

/* A fixed pool rather than simple_malloc: tasks are created and destroyed
 * constantly, and the bump allocator has no free(). */
static struct task pool[MAX_TASKS];
static struct task *queue;              /* sorted, most urgent first */
static int running_priority = PRIO_NONE;

int task_add(void (*callback)(void *data), void *data, int priority) {
    uint64_t daif = irq_disable_save();

    struct task *t = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (!pool[i].in_use) { t = &pool[i]; break; }
    }
    if (t == 0) {
        irq_restore(daif);
        return -1;
    }

    t->in_use   = 1;
    t->callback = callback;
    t->data     = data;
    t->priority = priority;

    /* Insert after every task at least as urgent, which keeps equal
     * priorities in arrival order. */
    struct task **link = &queue;
    while (*link && (*link)->priority <= priority) link = &(*link)->next;
    t->next = *link;
    *link = t;

    irq_restore(daif);
    return 0;
}

int task_pending_count(void) {
    uint64_t daif = irq_disable_save();
    int n = 0;
    for (struct task *t = queue; t; t = t->next) n++;
    irq_restore(daif);
    return n;
}

void task_run_pending(void) {
    /* Captured once: the loop toggles the mask while running callbacks, so a
     * per-iteration snapshot would restore whatever the last callback left. */
    uint64_t caller_daif;
    __asm__ volatile("mrs %0, daif" : "=r"(caller_daif));

    for (;;) {
        irq_disable();

        struct task *t = queue;
        /* Only run work more urgent than whatever this call interrupted;
         * anything else belongs to the outer invocation further down the
         * stack, which will get to it once this one returns. */
        if (t == 0 || t->priority >= running_priority) {
            irq_restore(caller_daif);
            return;
        }

        queue = t->next;
        int outer = running_priority;
        running_priority = t->priority;

        /* Interrupts on while the callback runs: a device interrupt can
         * arrive, enqueue a more urgent task, and preempt this one through a
         * nested call to this same loop. */
        irq_enable();
        t->callback(t->data);
        irq_disable();

        t->in_use = 0;
        running_priority = outer;
    }
}

#include "timer.h"
#include "irq.h"
#include "task.h"
#include "uart.h"
#include "mmio.h"

#define MAX_TIMERS    32
#define TIMER_MSG_MAX 64

/* The core timer is a single comparator, so any number of software timeouts
 * have to be multiplexed onto it: keep them ordered by expiry and always
 * program the hardware for the earliest one. */
struct soft_timer {
    uint64_t expire;                 /* absolute cntpct_el0 value */
    uint64_t scheduled_ms;           /* uptime when it was registered */
    void (*callback)(void *data);
    void *data;
    char msg[TIMER_MSG_MAX];
    int in_use;
    struct soft_timer *next;
};

static struct soft_timer pool[MAX_TIMERS];
static struct soft_timer *active;    /* sorted, earliest expiry first */
static uint64_t boot_count;
static int report_enabled;

static inline void write_cntp_ctl(uint64_t v) {
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(v));
}

static inline void write_cntp_tval(uint64_t v) {
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(v));
}

uint64_t timer_uptime_seconds(void) {
    uint64_t freq = timer_freq();
    if (freq == 0) return 0;
    return (timer_count() - boot_count) / freq;
}

uint64_t timer_uptime_ms(void) {
    uint64_t freq = timer_freq();
    if (freq == 0) return 0;
    /* At 62.5 MHz the numerator stays well inside 64 bits for any uptime this
     * kernel will ever see. */
    return ((timer_count() - boot_count) * 1000) / freq;
}

/* Milliseconds as seconds with three decimals. */
static void print_time(uint64_t ms) {
    uart_dec(ms / 1000);
    uart_send('.');
    uint64_t frac = ms % 1000;
    if (frac < 100) uart_send('0');
    if (frac < 10)  uart_send('0');
    uart_dec(frac);
    uart_send('s');
}

/* Programs the comparator for the head of the list, or leaves the timer
 * enabled but masked when nothing is pending. Interrupts must already be off. */
static void reprogram(void) {
    if (active == 0) {
        write_cntp_ctl(3);           /* ENABLE | IMASK */
        return;
    }

    uint64_t now = timer_count();
    uint64_t delta = (active->expire > now) ? active->expire - now : 1;

    /* cntp_tval_el0 is a signed 32-bit down-counter; a longer wait has to be
     * broken into several hops, which happens naturally because each expiry
     * reprograms from the list again. */
    if (delta > 0x7FFFFFFF) delta = 0x7FFFFFFF;

    write_cntp_tval(delta);
    write_cntp_ctl(1);               /* ENABLE, unmasked */
}

static struct soft_timer *alloc_timer(void) {
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!pool[i].in_use) {
            pool[i].in_use = 1;
            pool[i].next = 0;
            pool[i].msg[0] = '\0';
            return &pool[i];
        }
    }
    return 0;
}

static void insert(struct soft_timer *t) {
    struct soft_timer **link = &active;
    while (*link && (*link)->expire <= t->expire) link = &(*link)->next;
    t->next = *link;
    *link = t;
}

static int timer_add_ticks(void (*callback)(void *data), void *data, uint64_t ticks) {
    uint64_t daif = irq_disable_save();

    struct soft_timer *t = alloc_timer();
    if (t == 0) {
        irq_restore(daif);
        return -1;
    }

    t->scheduled_ms = timer_uptime_ms();
    t->expire       = timer_count() + ticks;
    t->callback     = callback;
    t->data         = data;
    insert(t);
    reprogram();

    irq_restore(daif);
    return 0;
}

int timer_add(void (*callback)(void *data), void *data, uint64_t after_seconds) {
    return timer_add_ticks(callback, data, after_seconds * timer_freq());
}

int timer_add_ms(void (*callback)(void *data), void *data, uint64_t after_ms) {
    return timer_add_ticks(callback, data, (after_ms * timer_freq()) / 1000);
}

/* Prints the message and redraws the prompt, so a timeout that lands while the
 * user is at the shell does not leave the line looking broken. */
static void message_callback(void *data) {
    struct soft_timer *t = (struct soft_timer *)data;
    uint64_t now = timer_uptime_ms();

    uart_puts("\n[timeout] ");
    uart_puts(t->msg);
    uart_puts("   (registered at ");
    print_time(t->scheduled_ms);
    uart_puts(", fired at ");
    print_time(now);
    uart_puts(", waited ");
    print_time(now - t->scheduled_ms);
    uart_puts(")\n# ");
}

int timer_add_message(const char *msg, uint64_t after_seconds) {
    uint64_t daif = irq_disable_save();

    struct soft_timer *t = alloc_timer();
    if (t == 0) {
        irq_restore(daif);
        return -1;
    }

    int i = 0;
    while (msg[i] && i < TIMER_MSG_MAX - 1) { t->msg[i] = msg[i]; i++; }
    t->msg[i] = '\0';

    t->scheduled_ms = timer_uptime_ms();
    t->expire       = timer_count() + after_seconds * timer_freq();
    t->callback     = message_callback;
    t->data         = t;
    insert(t);
    reprogram();

    irq_restore(daif);
    return 0;
}

int timer_pending_count(void) {
    uint64_t daif = irq_disable_save();
    int n = 0;
    for (struct soft_timer *t = active; t; t = t->next) n++;
    irq_restore(daif);
    return n;
}

/* Reached through the task queue, so it runs with interrupts enabled. */
static void run_expired(void *data) {
    struct soft_timer *t = (struct soft_timer *)data;
    if (t->callback) t->callback(t->data);
    t->in_use = 0;
}

void timer_irq(void) {
    uint64_t now = timer_count();

    while (active && active->expire <= now) {
        struct soft_timer *t = active;
        active = t->next;
        t->next = 0;

        /* Defer the callback instead of running it in interrupt context. If
         * the task queue is full, run it here rather than losing the timeout. */
        if (task_add(run_expired, t, TASK_PRIO_TIMER) != 0) {
            if (t->callback) t->callback(t->data);
            t->in_use = 0;
        }
    }

    reprogram();
}

/* Prints uptime, then rearms itself two seconds out. */
static void report_callback(void *data) {
    (void)data;
    if (!report_enabled) return;

    uart_puts("\n[timer] ");
    print_time(timer_uptime_ms());
    uart_puts(" since boot\n# ");

    timer_add(report_callback, 0, 2);
}

int timer_toggle_report(void) {
    report_enabled = !report_enabled;
    if (report_enabled) timer_add(report_callback, 0, 2);
    return report_enabled;
}

void timer_init(void) {
    boot_count = timer_count();

    /* Route the non-secure physical timer interrupt to this core. */
    mmio_write(CORE0_TIMER_IRQ_CTRL, 2);

    /* Let EL0 read the physical counter, so a user program can time itself
     * without a syscall. */
    uint64_t cntkctl;
    __asm__ volatile("mrs %0, cntkctl_el1" : "=r"(cntkctl));
    cntkctl |= 1;
    __asm__ volatile("msr cntkctl_el1, %0" :: "r"(cntkctl));

    /* Enabled but masked: nothing is scheduled yet, and an unmasked timer with
     * no deadline set would fire immediately. */
    write_cntp_ctl(3);
}

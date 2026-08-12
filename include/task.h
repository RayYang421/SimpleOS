#ifndef TASK_H
#define TASK_H

#include "types.h"

/* Deferred interrupt work.
 *
 * An interrupt handler should not do its processing inline: that keeps the
 * device masked for as long as the work takes. Instead it moves the data out
 * of the device, enqueues a task, and returns. Tasks then run with interrupts
 * re-enabled, so a later interrupt can preempt a lower-priority task that is
 * already running.
 *
 * Lower priority numbers run first. */
#define TASK_PRIO_TIMER  0
#define TASK_PRIO_UART   1
#define TASK_PRIO_IDLE   9

/* Enqueues work in priority order. Safe to call from an interrupt handler. */
int task_add(void (*callback)(void *data), void *data, int priority);

/* Runs queued tasks whose priority beats whatever is currently running, with
 * interrupts enabled so higher-priority work can preempt. Returns when only
 * lower-priority tasks remain. */
void task_run_pending(void);

/* Number of tasks waiting, for the shell's report. */
int task_pending_count(void);

#endif

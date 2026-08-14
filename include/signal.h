#ifndef SIGNAL_H
#define SIGNAL_H

#include "types.h"
#include "exception.h"

/* Checked on the way back to EL0. If something is pending, the frame about to
 * be restored is rewritten so the return lands in the handler instead. */
void signal_check(struct trap_frame *tf);

void signal_register(int sig, void (*handler)(void));
void signal_send(int pid, int sig);

/* Puts back the frame that was saved when the handler was entered. */
void signal_return(struct trap_frame *tf);

#endif

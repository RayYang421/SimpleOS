#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"
#include "exception.h"

/* The number goes in x8, arguments in x0.., the result comes back in x0, and
 * the call is made with `svc 0`. */
#define SYS_GETPID      0
#define SYS_UART_READ   1
#define SYS_UART_WRITE  2
#define SYS_EXEC        3
#define SYS_FORK        4
#define SYS_EXIT        5
#define SYS_MBOX_CALL   6
#define SYS_KILL        7
#define SYS_SIGNAL      8
#define SYS_SIGKILL     9
#define SYS_SIGRETURN   10

/* Well clear of the lab's numbering, so the lab 3 exception demo keeps
 * working alongside the real calls. */
#define SYS_DEMO_REPORT 100
#define SYS_DEMO_LEAVE  101

void syscall_dispatch(struct trap_frame *tf);

/* Starts a user program from the initramfs in the calling thread. Returns 0 on
 * success, -1 if the file is missing or memory runs out. */
int do_exec(struct trap_frame *tf, const char *name);

#endif

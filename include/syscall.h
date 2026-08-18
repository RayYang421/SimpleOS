#ifndef SYSCALL_H
#define SYSCALL_H

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
#define SYS_MMAP        10
#define SYS_OPEN        11
#define SYS_CLOSE       12
#define SYS_FWRITE      13
#define SYS_FREAD       14
#define SYS_MKDIR       15
#define SYS_MOUNT       16
#define SYS_CHDIR       17
#define SYS_LSEEK64     18
#define SYS_IOCTL       19
#define SYS_SYNC        20

/* The lab numbers everything up to sync; sigreturn is the kernel's own
 * business, reached only from the trampoline a signal handler returns to, so
 * it takes the next free number. */
#define SYS_SIGRETURN   21

/* Well clear of the lab's numbering, so the lab 3 exception demo keeps
 * working alongside the real calls. */
#define SYS_DEMO_REPORT 100
#define SYS_DEMO_LEAVE  101

#ifndef __ASSEMBLER__

#include "types.h"
#include "exception.h"

void syscall_dispatch(struct trap_frame *tf);

/* Replaces the calling thread's address space with a program from the
 * initramfs and points its trap frame at the new entry. Returns 0 on success,
 * -1 if the file is missing or memory runs out. */
int do_exec(struct trap_frame *tf, const char *name);

#endif /* __ASSEMBLER__ */

#endif

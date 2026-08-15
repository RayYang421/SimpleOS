#include "shell.h"
#include "uart.h"
#include "string.h"
#include "cpio.h"
#include "fdt.h"
#include "mm.h"
#include "mmio.h"
#include "irq.h"
#include "timer.h"
#include "task.h"
#include "exception.h"
#include "page.h"
#include "sched.h"
#include "syscall.h"
#include "signal.h"
#include "mbox.h"
#include "vm.h"
#include "mmu.h"

#define CMD_MAX   256
#define ARGV_MAX  8

/* Power management watchdog, used to reset the board. */
#define PM_PASSWORD 0x5A000000u
#define PM_RSTC     (MMIO_BASE + 0x0010001C)
#define PM_WDOG     (MMIO_BASE + 0x00100024)
#define PM_RSTC_FULL_RESET 0x20u

/* Echoes as it goes. Input past the end of the buffer is dropped rather than
 * overflowing it. */
static void read_line(char *buf, int max) {
    int i = 0;

    for (;;) {
        char c = uart_getc();

        if (c == '\n') {
            uart_puts("\n");
            break;
        }

        if (c == '\b' || c == 0x7F) {        /* BS or DEL */
            if (i > 0) {
                i--;
                uart_puts("\b \b");          /* rub the character off the screen */
            }
            continue;
        }

        if (c < ' ' || c > '~') continue;

        if (i < max - 1) {
            buf[i++] = c;
            uart_send(c);
        }
    }

    buf[i] = '\0';
}

/* Splits the line in place, overwriting the separators. */
static int tokenize(char *line, char **argv, int max_args) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max_args) {
        while (*p == ' ' || *p == '\t') *p++ = '\0';
        if (*p == '\0') break;

        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }

    if (*p) *p = '\0';
    return argc;
}

static void cmd_help(void) {
    uart_puts("help                     : print all available commands\n");
    uart_puts("hello                    : print Hello World!\n");
    uart_puts("ls                       : list files in the initramfs\n");
    uart_puts("cat <file>               : print the contents of a file\n");
    uart_puts("malloc <size>            : allocate <size> bytes with simple_malloc\n");
    uart_puts("dtb                      : show devicetree info\n");
    uart_puts("info                     : show board memory info from the devicetree\n");
    uart_puts("exc                      : run a user program at EL0 that issues SVC\n");
    uart_puts("timer                    : toggle the two-second uptime report\n");
    uart_puts("setTimeout <msg> <secs>  : print <msg> after <secs> seconds\n");
    uart_puts("irqinfo                  : interrupt, timer and task statistics\n");
    uart_puts("tasktest                 : show a timer task preempting a long one\n");
    uart_puts("meminfo                  : allocator statistics\n");
    uart_puts("memtest                  : demo split, merge, chunking and reuse\n");
    uart_puts("palloc <order>           : allocate 2^order page frames\n");
    uart_puts("pfree <addr>             : free a page block\n");
    uart_puts("kmalloc <size>           : allocate from the dynamic allocator\n");
    uart_puts("kfree <addr>             : free a dynamic allocation\n");
    uart_puts("ps                       : list threads\n");
    uart_puts("kthread [n]              : create n kernel threads that yield\n");
    uart_puts("exec <file>              : run a user program from the initramfs\n");
    uart_puts("kill <pid>               : send SIGKILL to a thread\n");
    uart_puts("mbox                     : board info through the VideoCore mailbox\n");
    uart_puts("vm [pid]                 : show a process's address space\n");
    uart_puts("reboot                   : reset the board\n");
}

/* Prints every property of /chosen, which is where the loader records the
 * initramfs location and kernel command line. */
static void chosen_callback(const char *node, const char *prop,
                            const void *val, uint32_t len, void *arg) {
    (void)arg;
    if (strcmp(node, "chosen") != 0) return;

    uart_puts("  ");
    uart_puts(prop);
    uart_puts(" = ");

    if (len == 4) {
        uart_hex(fdt_be32(val));
    } else if (len == 8) {
        uart_hex(((uint64_t)fdt_be32(val) << 32) | fdt_be32((const uint8_t *)val + 4));
    } else {
        uart_puts("\"");
        uart_write((const char *)val, len ? len - 1 : 0);   /* drop trailing NUL */
        uart_puts("\"");
    }
    uart_puts("\n");
}

static void cmd_dtb(void) {
    uint64_t base = fdt_get_base();

    if (base == 0) {
        uart_puts("no devicetree passed in x0 "
                  "(run QEMU with -dtb bcm2710-rpi-3-b-plus.dtb)\n");
    } else {
        uart_puts("devicetree base: ");
        uart_hex(base);
        uart_puts("\n/chosen:\n");
        fdt_traverse(chosen_callback, 0);
    }

    uart_puts("initramfs base : ");
    uart_hex(PA(cpio_get_base()));
    uart_puts(cpio_valid() ? "  (valid cpio archive)\n" : "  (no archive here)\n");
}

/* The /memory node's reg property is a list of (address, size) pairs, each
 * cell pair sized by #address-cells/#size-cells; on the Rpi3 both are 1. */
static void memory_callback(const char *node, const char *prop,
                            const void *val, uint32_t len, void *arg) {
    (void)arg;
    if (!fdt_is_memory_node(node)) return;
    if (strcmp(prop, "reg") != 0 || len < 8) return;

    uart_puts("  memory: base ");
    uart_hex(fdt_be32(val));
    uart_puts("  size ");
    uart_hex(fdt_be32((const uint8_t *)val + 4));
    uart_puts("\n");
}

static void cmd_info(void) {
    if (fdt_get_base() == 0) {
        uart_puts("no devicetree available\n");
        return;
    }
    uart_puts("board info:\n");
    fdt_traverse(memory_callback, 0);
}

static void cmd_malloc(int argc, char **argv) {
    uint64_t size;

    if (argc < 2 || !parse_uint(argv[1], &size)) {
        uart_puts("usage: malloc <size>   (decimal, or 0x-prefixed hex)\n");
        return;
    }

    void *p = simple_malloc((size_t)size);
    if (p == 0) {
        uart_puts("simple_malloc failed: out of heap\n");
    } else {
        uart_puts("allocated ");
        uart_dec(size);
        uart_puts(" bytes at ");
        uart_hex((uint64_t)(uintptr_t)p);
        uart_puts("\n");
    }

    size_t used, total;
    simple_malloc_stats(&used, &total);
    uart_puts("heap: ");
    uart_dec(used);
    uart_puts(" / ");
    uart_dec(total);
    uart_puts(" bytes used\n");
}

/* EL0 cannot execute the kernel's text once the MMU is on, so even this demo
 * needs an address space: one holding the shared page the program lives in and
 * a stack. The shell thread borrows it for the excursion and gives it back. */
static void cmd_exc(void) {
    struct thread *t = current();

    if (vm_new_address_space(t, 0, 0) != 0) {
        uart_puts("out of memory\n");
        return;
    }
    vm_switch(t->pgd);

    uart_puts("dropping to EL0; the user program issues SVC five times\n");
    enter_el0((void *)vm_shared_va((void *)user_program), (void *)USER_STACK_TOP);

    vm_destroy(t);
    vm_switch(0);

    uart_puts("returned to the kernel, now at EL");
    uart_dec(current_el());
    uart_puts("\n");
}

static void cmd_set_timeout(int argc, char **argv) {
    uint64_t seconds;

    if (argc < 3 || !parse_uint(argv[2], &seconds)) {
        uart_puts("usage: setTimeout <message> <seconds>\n");
        return;
    }

    if (timer_add_message(argv[1], seconds) != 0) {
        uart_puts("no free timer slots\n");
        return;
    }

    uart_puts("registered \"");
    uart_puts(argv[1]);
    uart_puts("\" for ");
    uart_dec(seconds);
    uart_puts("s from now (at ");
    uart_dec(timer_uptime_seconds());
    uart_puts("s); the shell stays usable meanwhile\n");
}

static volatile int preempted;

static void urgent_task(void *data) {
    (void)data;
    uart_puts("\n    [prio 0] timer task ran at ");
    uart_dec(timer_uptime_ms());
    uart_puts(" ms, interrupting the long task\n");
    preempted = 1;
}

static void long_task(void *data) {
    (void)data;

    uart_puts("    [prio 9] long task started at ");
    uart_dec(timer_uptime_ms());
    uart_puts(" ms, busy for ~2s\n");

    uint64_t end = timer_count() + 2 * timer_freq();
    while (timer_count() < end) { }

    uart_puts("    [prio 9] long task finished at ");
    uart_dec(timer_uptime_ms());
    uart_puts(" ms, was preempted: ");
    uart_puts(preempted ? "yes\n" : "no\n");
}

/* A long low-priority task runs with interrupts enabled; a timer scheduled to
 * expire in the middle of it enqueues a top-priority task, which
 * task_run_pending runs immediately rather than waiting for the long one. */
static void cmd_tasktest(void) {
    preempted = 0;

    uart_puts("queueing a 2s priority-9 task and a 1s timer at priority 0\n");
    timer_add(urgent_task, 0, 1);
    task_add(long_task, 0, TASK_PRIO_IDLE);

    /* Nothing has interrupted us, so drain the queue here -- this is the
     * "run tasks when the system is idle" path. */
    task_run_pending();
}

static void cmd_irqinfo(void) {
    uint64_t rx, tx, irqs;
    uart_async_stats(&rx, &tx, &irqs);

    uart_puts("uptime          : ");
    uart_dec(timer_uptime_seconds());
    uart_puts(" s\nuart interrupts : ");
    uart_dec(irqs);
    uart_puts("\nbytes received  : ");
    uart_dec(rx);
    uart_puts("\nbytes sent      : ");
    uart_dec(tx);
    uart_puts("\ntimers pending  : ");
    uart_dec(timer_pending_count());
    uart_puts("\ntasks queued    : ");
    uart_dec(task_pending_count());
    uart_puts("\n");
}

static void cmd_meminfo(void) {
    size_t used, total;
    simple_malloc_stats(&used, &total);

    uart_puts("startup allocator: ");
    uart_dec(used);
    uart_puts(" / ");
    uart_dec(total);
    uart_puts(" bytes used\n");

    page_report();
    kmalloc_report();
}

static void cmd_palloc(int argc, char **argv) {
    uint64_t order;

    if (argc < 2 || !parse_uint(argv[1], &order) || order > MAX_ORDER) {
        uart_puts("usage: palloc <order>   (0..");
        uart_dec(MAX_ORDER);
        uart_puts(", allocates 2^order frames)\n");
        return;
    }

    int was = page_set_log(1);
    void *p = page_alloc((int)order);
    page_set_log(was);

    if (p == 0) {
        uart_puts("page_alloc failed\n");
        return;
    }

    uart_puts("got ");
    uart_hex((uint64_t)(uintptr_t)p);
    uart_puts("  ");
    uart_dec((1UL << order) * (PAGE_SIZE / 1024));
    uart_puts(" KiB\n");
}

static void cmd_pfree(int argc, char **argv) {
    uint64_t addr;

    if (argc < 2 || !parse_uint(argv[1], &addr)) {
        uart_puts("usage: pfree <address>\n");
        return;
    }

    int was = page_set_log(1);
    page_free((void *)(uintptr_t)addr);
    page_set_log(was);
}

static void cmd_kmalloc(int argc, char **argv) {
    uint64_t size;

    if (argc < 2 || !parse_uint(argv[1], &size)) {
        uart_puts("usage: kmalloc <size>\n");
        return;
    }

    int was = kmalloc_set_log(1);
    void *p = kmalloc((size_t)size);
    kmalloc_set_log(was);

    if (p == 0) uart_puts("kmalloc failed\n");
}

static void cmd_kfree(int argc, char **argv) {
    uint64_t addr;

    if (argc < 2 || !parse_uint(argv[1], &addr)) {
        uart_puts("usage: kfree <address>\n");
        return;
    }

    int was = kmalloc_set_log(1);
    kfree((void *)(uintptr_t)addr);
    kmalloc_set_log(was);
}

/* Walks through the behaviour the allocators are meant to show: a block being
 * split down to size, the halves merging back on free, several chunks coming
 * out of one page frame, and that frame going back to the buddy system once
 * the last chunk is returned. The frame count either side proves nothing
 * leaked. */
#define DEMO_HOLD_MAX  64
#define DEMO_DRAIN_TO  4

static void *demo_held[DEMO_HOLD_MAX];

static void cmd_memtest(void) {
    uint64_t before, after;
    page_stats(0, &before, 0);

    /* Startup leaves small blocks free around the reserved regions, so a small
     * request would be satisfied outright and never split anything. Taking
     * every exact-size block below DEMO_DRAIN_TO first forces the next request
     * to come out of a larger block. Each of these finds an exact match, so
     * draining does not itself split. */
    int quiet = page_set_log(0);
    int held = 0;
    for (int order = 0; order <= DEMO_DRAIN_TO && held < DEMO_HOLD_MAX; order++) {
        int n = page_order_count(order);
        for (int i = 0; i < n && held < DEMO_HOLD_MAX; i++) {
            void *p = page_alloc(order);
            if (p == 0) break;
            demo_held[held++] = p;
        }
    }
    page_set_log(1);
    int klog = kmalloc_set_log(1);

    uart_puts("\n[1] page allocator: one frame, taken from a larger block\n");
    void *small = page_alloc(0);

    uart_puts("\n[2] page allocator: the same frame freed, buddies merging back\n");
    page_free(small);

    uart_puts("\n[3] dynamic allocator: chunks cut from one page frame\n");
    void *x = kmalloc(24);
    void *y = kmalloc(24);
    void *z = kmalloc(100);

    uint64_t px = (uint64_t)(uintptr_t)x & ~(PAGE_SIZE - 1);
    uint64_t py = (uint64_t)(uintptr_t)y & ~(PAGE_SIZE - 1);
    uint64_t pz = (uint64_t)(uintptr_t)z & ~(PAGE_SIZE - 1);
    uart_puts("  24-byte chunks share a frame: ");
    uart_puts(px == py ? "yes\n" : "no\n");
    uart_puts("  100-byte chunk uses a different bin: ");
    uart_puts(pz != px ? "yes\n" : "no\n");

    uart_puts("\n[4] dynamic allocator: emptied frames go back to the buddy system\n");
    kfree(x);
    kfree(y);
    kfree(z);

    kmalloc_set_log(klog);
    page_set_log(0);
    while (held > 0) page_free(demo_held[--held]);
    page_set_log(quiet);

    page_stats(0, &after, 0);
    uart_puts("\nfree frames before ");
    uart_dec(before);
    uart_puts(", after ");
    uart_dec(after);
    uart_puts(" -> leaked ");
    uart_dec(before >= after ? before - after : 0);
    uart_puts(" frames\n");
}

/* --- lab 5 --------------------------------------------------------------- */

static void demo_kthread(void) {
    struct thread *t = current();

    for (int i = 0; i < 5; i++) {
        /* A line is several calls, and each one only holds off preemption for
         * its own duration, so the whole line needs holding together. */
        preempt_disable();
        uart_puts("  thread ");
        uart_dec(t->pid);
        uart_puts(" iteration ");
        uart_dec(i);
        uart_puts("\n");
        preempt_enable();

        for (volatile int d = 0; d < 200000; d++) { }
        schedule();             /* yield, so the interleaving is visible */
    }
}

static void cmd_kthread(int argc, char **argv) {
    uint64_t n = 3;

    if (argc >= 2 && !parse_uint(argv[1], &n)) {
        uart_puts("usage: kthread [count]\n");
        return;
    }
    if (n == 0 || n > 8) n = 3;

    uart_puts("creating ");
    uart_dec(n);
    uart_puts(" kernel threads; their output interleaves as they yield\n");

    for (uint64_t i = 0; i < n; i++) {
        if (thread_create(demo_kthread) == 0) {
            uart_puts("thread_create failed\n");
            return;
        }
    }
}

/* exec runs in a thread of its own rather than replacing the shell, so the
 * shell stays usable while the user program runs -- which is the point of the
 * timer preemption. */
static char exec_name[64];

static void exec_thread(void) {
    struct thread *t = current();

    if (do_exec(t->tf, exec_name) != 0) {
        uart_puts("exec: cannot load \"");
        uart_puts(exec_name);
        uart_puts("\" from the initramfs\n");
        thread_exit(-1);
    }

    enter_user_mode(t->tf);     /* does not return */
}

static void cmd_exec(int argc, char **argv) {
    if (argc < 2) {
        uart_puts("usage: exec <file>   (a flat binary in the initramfs)\n");
        return;
    }

    int i = 0;
    while (argv[1][i] && i < (int)sizeof(exec_name) - 1) {
        exec_name[i] = argv[1][i];
        i++;
    }
    exec_name[i] = '\0';

    if (thread_create(exec_thread) == 0) uart_puts("out of memory\n");
}

static void cmd_kill(int argc, char **argv) {
    uint64_t pid;

    if (argc < 2 || !parse_uint(argv[1], &pid)) {
        uart_puts("usage: kill <pid>\n");
        return;
    }

    if (thread_by_pid((int)pid) == 0) {
        uart_puts("no such thread\n");
        return;
    }

    signal_send((int)pid, SIGKILL);
    uart_puts("SIGKILL sent to pid ");
    uart_dec(pid);
    uart_puts("\n");
}

static void cmd_vm(int argc, char **argv) {
    uint64_t pid;

    if (argc >= 2) {
        if (!parse_uint(argv[1], &pid)) {
            uart_puts("usage: vm [pid]\n");
            return;
        }

        struct thread *t = thread_by_pid((int)pid);
        if (t == 0) uart_puts("no such thread\n");
        else        vm_report(t);
        return;
    }

    /* Every process at once, held against preemption so none of them can exit
     * and free its regions half-way through the walk. */
    preempt_disable();

    uart_puts("live address spaces:\n");

    int found = 0;
    for (struct thread *t = thread_iter(0); t; t = thread_iter(t)) {
        if (t->pgd == 0) continue;
        vm_report(t);
        found++;
    }

    preempt_enable();

    if (found == 0) uart_puts("no process has an address space right now\n");
}

static void cmd_mbox(void) {
    uint32_t revision, base, size;

    if (mbox_board_revision(&revision) == 0) {
        uart_puts("board revision: ");
        uart_hex(revision);
        uart_puts("\n");
    } else {
        uart_puts("board revision: mailbox call failed\n");
    }

    if (mbox_arm_memory(&base, &size) == 0) {
        uart_puts("arm memory    : base ");
        uart_hex(base);
        uart_puts("  size ");
        uart_hex(size);
        uart_puts("\n");
    } else {
        uart_puts("arm memory    : mailbox call failed\n");
    }
}

static void cmd_reboot(void) {
    uart_puts("rebooting...\n");
    uart_flush();
    mmio_write(PM_WDOG, PM_PASSWORD | 100);                     /* timeout ticks */
    mmio_write(PM_RSTC, PM_PASSWORD | PM_RSTC_FULL_RESET);
    for (;;) { }
}

void shell(void) {
    char line[CMD_MAX];
    char *argv[ARGV_MAX];

    uart_puts("\nWelcome to my-os shell! Type 'help' for commands.\n");

    for (;;) {
        uart_puts("# ");
        read_line(line, sizeof(line));

        int argc = tokenize(line, argv, ARGV_MAX);
        if (argc == 0) continue;

        if      (strcmp(argv[0], "help")   == 0) cmd_help();
        else if (strcmp(argv[0], "hello")  == 0) uart_puts("Hello World!\n");
        else if (strcmp(argv[0], "ls")     == 0) cpio_ls();
        else if (strcmp(argv[0], "dtb")    == 0) cmd_dtb();
        else if (strcmp(argv[0], "info")   == 0) cmd_info();
        else if (strcmp(argv[0], "malloc") == 0) cmd_malloc(argc, argv);
        else if (strcmp(argv[0], "exc")    == 0) cmd_exc();
        else if (strcmp(argv[0], "irqinfo") == 0) cmd_irqinfo();
        else if (strcmp(argv[0], "tasktest") == 0) cmd_tasktest();
        else if (strcmp(argv[0], "ps")      == 0) thread_list();
        else if (strcmp(argv[0], "kthread") == 0) cmd_kthread(argc, argv);
        else if (strcmp(argv[0], "exec")    == 0) cmd_exec(argc, argv);
        else if (strcmp(argv[0], "kill")    == 0) cmd_kill(argc, argv);
        else if (strcmp(argv[0], "mbox")    == 0) cmd_mbox();
        else if (strcmp(argv[0], "vm")      == 0) cmd_vm(argc, argv);
        else if (strcmp(argv[0], "meminfo") == 0) cmd_meminfo();
        else if (strcmp(argv[0], "memtest") == 0) cmd_memtest();
        else if (strcmp(argv[0], "palloc")  == 0) cmd_palloc(argc, argv);
        else if (strcmp(argv[0], "pfree")   == 0) cmd_pfree(argc, argv);
        else if (strcmp(argv[0], "kmalloc") == 0) cmd_kmalloc(argc, argv);
        else if (strcmp(argv[0], "kfree")   == 0) cmd_kfree(argc, argv);
        else if (strcmp(argv[0], "reboot") == 0) cmd_reboot();
        else if (strcmp(argv[0], "setTimeout") == 0) cmd_set_timeout(argc, argv);
        else if (strcmp(argv[0], "timer")  == 0) {
            uart_puts(timer_toggle_report()
                      ? "uptime report on (every two seconds)\n"
                      : "uptime report off\n");
        }
        else if (strcmp(argv[0], "cat")    == 0) {
            if (argc < 2) {
                uart_puts("usage: cat <file>\n");
            } else if (cpio_cat(argv[1]) != 0) {
                uart_puts("cat: ");
                uart_puts(argv[1]);
                uart_puts(": no such file\n");
            }
        }
        else {
            uart_puts("Unknown command: ");
            uart_puts(argv[0]);
            uart_puts("\n");
        }
    }
}

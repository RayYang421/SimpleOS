#include "shell.h"
#include "uart.h"
#include "string.h"
#include "cpio.h"
#include "fdt.h"
#include "mm.h"
#include "mmio.h"

#define CMD_MAX   256
#define ARGV_MAX  8

/* Power management watchdog, used to reset the board. */
#define PM_PASSWORD 0x5A000000u
#define PM_RSTC     (MMIO_BASE + 0x0010001C)
#define PM_WDOG     (MMIO_BASE + 0x00100024)
#define PM_RSTC_FULL_RESET 0x20u

/* Reads one line, echoing as it goes so the user can see what they type.
 * Handles backspace; anything past the buffer is dropped rather than
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

        if (c < ' ' || c > '~') continue;    /* ignore other control bytes */

        if (i < max - 1) {
            buf[i++] = c;
            uart_send(c);
        }
    }

    buf[i] = '\0';
}

/* Splits the line in place on spaces. Returns the argument count. */
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
    uart_puts("help           : print all available commands\n");
    uart_puts("hello          : print Hello World!\n");
    uart_puts("ls             : list files in the initramfs\n");
    uart_puts("cat <file>     : print the contents of a file\n");
    uart_puts("malloc <size>  : allocate <size> bytes with simple_malloc\n");
    uart_puts("dtb            : show devicetree info\n");
    uart_puts("info           : show board memory info from the devicetree\n");
    uart_puts("reboot         : reset the board\n");
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
    uart_hex((uint64_t)(uintptr_t)cpio_get_base());
    uart_puts(cpio_valid() ? "  (valid cpio archive)\n" : "  (no archive here)\n");
}

/* The /memory node's reg property is a list of (address, size) pairs, each
 * cell pair sized by #address-cells/#size-cells; on the Rpi3 both are 1. */
static void memory_callback(const char *node, const char *prop,
                            const void *val, uint32_t len, void *arg) {
    (void)arg;
    if (strncmp(node, "memory", 6) != 0) return;
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

static void cmd_reboot(void) {
    uart_puts("rebooting...\n");
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
        else if (strcmp(argv[0], "reboot") == 0) cmd_reboot();
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

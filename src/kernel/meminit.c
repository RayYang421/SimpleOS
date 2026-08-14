#include "mm.h"
#include "page.h"
#include "fdt.h"
#include "cpio.h"
#include "uart.h"

/* Laid down by the linker script; __end is past the startup heap, so the two
 * together cover everything the kernel image occupies. */
extern char _start[];
extern char __end[];

/* The Rpi3 firmware leaves the secondary cores spinning on mailboxes down
 * here, so this must never be handed out. */
#define SPIN_TABLE_TOP    0x1000UL

/* start.S sets sp to _start and the stack grows down from there, into memory
 * the buddy system would otherwise consider free -- it would thread its
 * free-list nodes straight through the running stack. Reserve a generous span
 * below the image; the deepest path here is a handful of nested exception
 * frames. */
#define KERNEL_STACK_SIZE 0x10000UL

/* Used only when the devicetree does not describe memory. */
#define FALLBACK_MEM_BASE 0x0UL
#define FALLBACK_MEM_SIZE 0x3C000000UL

void mem_init(void) {
    uint64_t base, size;

    if (fdt_get_memory(&base, &size) != 0) {
        base = FALLBACK_MEM_BASE;
        size = FALLBACK_MEM_SIZE;
        uart_puts("memory: no /memory node, assuming ");
    } else {
        uart_puts("memory: ");
    }

    uart_hex(base);
    uart_puts(" .. ");
    uart_hex(base + size);
    uart_puts("  (");
    uart_dec(size >> 20);
    uart_puts(" MiB, ");
    uart_dec(size >> PAGE_SHIFT);
    uart_puts(" frames)\n");

    /* The frame array comes from the startup allocator, before the buddy
     * system it describes can hand out anything. */
    page_init(base, size);

    memory_reserve(0, SPIN_TABLE_TOP, "spin tables");
    memory_reserve((uint64_t)(uintptr_t)_start - KERNEL_STACK_SIZE,
                   (uint64_t)(uintptr_t)_start, "kernel stack");
    memory_reserve((uint64_t)(uintptr_t)_start, (uint64_t)(uintptr_t)__end,
                   "kernel image");

    /* Inside the kernel image span above, but reserved by name so the
     * dependency is explicit rather than incidental. */
    uint64_t heap_start, heap_end;
    startup_alloc_range(&heap_start, &heap_end);
    memory_reserve(heap_start, heap_end, "startup allocator + frame array");

    uint64_t dtb = fdt_get_base();
    if (dtb) memory_reserve(dtb, dtb + fdt_get_totalsize(), "devicetree blob");

    uint64_t initrd_start, initrd_end;
    if (fdt_get_initrd(&initrd_start, &initrd_end) == 0) {
        memory_reserve(initrd_start, initrd_end, "initramfs (from devicetree)");
    } else if (cpio_valid()) {
        /* No devicetree to ask, so measure the archive itself. */
        initrd_start = (uint64_t)(uintptr_t)cpio_get_base();
        memory_reserve(initrd_start, initrd_start + cpio_size(),
                       "initramfs (measured)");
    }

    /* Everything still unreserved becomes buddy blocks. */
    page_finalize();

    page_report();
}

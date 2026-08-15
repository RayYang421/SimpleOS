/* The Rpi3 firmware (and QEMU's -kernel) load a raw AArch64 image at physical
 * 0x80000, but the kernel runs with the MMU on and lives in the upper half of
 * the address space, so it is *linked* for the virtual address that physical
 * 0x80000 maps to. start.S builds the tables and jumps up here before anything
 * touches a linked address. */
ENTRY(_start)

KERNEL_VA_BASE = 0xffff000000000000;
KERNEL_PA      = 0x80000;

SECTIONS
{
    . = KERNEL_VA_BASE + KERNEL_PA;

    .text : {
        KEEP(*(.text.boot))   /* boot code first: _start must be at 0x80000 */
        *(.text .text.*)
    }

    /* Code that runs at EL0 -- the signal trampoline and the exception demo --
     * gets a page of its own, mapped into every user address space. It cannot
     * live with the rest of the kernel's text, because a page mapped for EL0
     * would expose everything else sharing it. */
    . = ALIGN(4096);
    __user_page_start = .;
    .usertext : { KEEP(*(.usertext)) }
    . = ALIGN(4096);
    __user_page_end = .;

    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }

    /* start.S clears bss eight bytes at a time, so both ends must be aligned
     * and the span a multiple of 8. */
    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(16);
    __bss_end = .;

    /* Heap for simple_malloc. It lives past the image rather than inside a
     * .bss array so it costs nothing in the binary and can be resized here.
     * It has to hold the page allocator's per-frame array and reference
     * counts, which together come to about 3 bytes per frame of RAM. */
    . = ALIGN(16);
    __heap_start = .;
    . = . + 0x200000;      /* 2 MiB */
    __heap_end = .;

    __end = .;
}

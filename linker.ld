/* The Rpi3 firmware (and QEMU's -kernel) load a raw AArch64 image at 0x80000,
 * so that is where the kernel must be linked to run. */
ENTRY(_start)

SECTIONS
{
    . = 0x80000;

    .text : {
        KEEP(*(.text.boot))   /* boot code first: _start must be at 0x80000 */
        *(.text .text.*)
    }

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
     * .bss array so it costs nothing in the binary and can be resized here. */
    . = ALIGN(16);
    __heap_start = .;
    . = . + 0x100000;      /* 1 MiB */
    __heap_end = .;

    __end = .;
}

/* User programs are loaded as flat binaries at whatever address the kernel
 * finds room at, and it jumps to the first byte -- so _start has to be exactly
 * there. Without this, the compiler is free to emit some other function first
 * and the jump lands mid-routine. */
ENTRY(_start)

SECTIONS
{
    . = 0;

    .text : {
        KEEP(*(.text.entry))
        *(.text .text.*)
    }

    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }

    . = ALIGN(16);
    .bss    : { *(.bss .bss.*) *(COMMON) }
}

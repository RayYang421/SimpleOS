
ENTRY(_start)

SECTIONS
{
    . = 0x60000;

    __bootloader_start = .;

    .text : {
        KEEP(*(.text.boot))   /* _start must be the first byte of the image */
        *(.text .text.*)
    }

    .rodata : { *(.rodata .rodata.*) }
    .data   : { *(.data .data.*) }

    . = ALIGN(16);
    __bss_start = .;
    .bss : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(16);
    __bss_end = .;

    __bootloader_end = .;
}

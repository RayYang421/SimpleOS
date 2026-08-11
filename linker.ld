SECTIONS
{
    . = 0x80000;
    .text : {
        KEEP(*(.text.boot))   /* boot code first */
        *(.text)
    }
    .rodata : { *(.rodata) }
    .data   : { *(.data) }

    . = ALIGN(8);
    __bss_start = .;
    .bss : { *(.bss) *(COMMON) }
    __bss_end = .;
}
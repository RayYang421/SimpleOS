
SECTIONS{
    . = 0x60000;    /*bootloader addr*/
    .text : {
        KEEP(*(.text.boot))
        *(.text)
    }
    .rodata : { *(.rodata) }
    .data   : { *(.data) }
    . = ALIGN(8);
    __bss_start = .;
    .bss : { *(.bss) *(COMMON) }
    __bss_end = .;
}
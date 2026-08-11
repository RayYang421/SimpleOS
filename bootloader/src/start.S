.section ".text.boot"

.global _start

_start:
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, primary
hang:
    wfe
    b       hang

primary:

    ldr     x0, =_start
    mov     sp, x0

    ldr     x0, =__bss_start
    ldr     x1, =__bss_end

clear_bss:
    cmp     x0, x1
    b.ge    run_main
    str     xzr, [x0], #8
    b       clear_bss

run_main:
    bl      main
    b       hang
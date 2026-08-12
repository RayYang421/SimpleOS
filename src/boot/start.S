.section ".text.boot"

.global _start

_start:
    /* x0 holds the devicetree pointer from the bootloader (or the firmware
     * when booted directly). Stash it before anything else clobbers x0 -- the
     * mpidr check below would otherwise destroy it. */
    mov     x20, x0

    mrs     x1, mpidr_el1
    and     x1, x1, #0xFF
    cbz     x1, primary_core
hang:
    wfe
    b       hang

primary_core:
    /* Stack grows down from the kernel's load address; everything below
     * 0x80000 is free once the bootloader has handed off. */
    ldr     x1, =_start
    mov     sp, x1

    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
clear_bss:
    cmp     x1, x2
    b.ge    run_main
    str     xzr, [x1], #8
    b       clear_bss

run_main:
    mov     x0, x20                 /* main(uint64_t dtb_addr) */
    bl      main
    b       hang

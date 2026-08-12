.section ".text.boot"

.global _start

_start:
    /* The firmware passes a devicetree pointer in x0. Save it first -- it has
     * to survive relocation and be handed to the kernel unchanged. */
    mov     x20, x0

    mrs     x1, mpidr_el1
    and     x1, x1, #0xFF
    cbz     x1, primary

/* wfi rather than wfe: nothing signals these cores, and an emulator treats wfe
 * as a yield, so a wfe loop burns a host thread per parked core. */
hang:
    wfi
    b       hang

primary:
    adr     x1, _start                  /* runtime address */
    ldr     x2, =__bootloader_start     /* link address */
    ldr     x3, =__bootloader_end

    cmp     x1, x2
    b.eq    at_link_address

    sub     x3, x3, x2                  /* image size, a multiple of 16 */
    mov     x4, x1                      /* src */
    mov     x5, x2                      /* dst */
relocate:
    cbz     x3, relocate_done
    ldr     x6, [x4], #8
    str     x6, [x5], #8
    sub     x3, x3, #8
    b       relocate

relocate_done:
    /* Caches and the MMU are off this early, so the copy is already visible to
     * the instruction fetcher; the barriers make that explicit and cost
     * nothing at boot. */
    dsb     sy
    ic      iallu
    isb

    /* Jump to the copy. This must be an absolute branch: a PC-relative one
     * would keep executing at the old address. */
    ldr     x7, =at_link_address
    br      x7

at_link_address:
    /* Stack grows down from the bootloader's base, into the free memory below
     * it -- never up into the kernel's 0x80000 landing zone. */
    ldr     x1, =__bootloader_start
    mov     sp, x1

    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
clear_bss:
    cmp     x1, x2
    b.ge    run_main
    str     xzr, [x1], #8
    b       clear_bss

run_main:
    mov     x0, x20                     /* main(uint64_t dtb_addr) */
    bl      main
    b       hang

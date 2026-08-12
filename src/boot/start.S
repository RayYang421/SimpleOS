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

/* Parked with wfi rather than wfe: nothing ever sends these cores an event, and
 * an emulator implements wfe as a yield rather than a halt, so a wfe loop would
 * spin at full speed instead of idling. wfi halts until an interrupt, which
 * these cores are never sent. */
hang:
    wfi
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
    b.ge    check_el
    str     xzr, [x1], #8
    b       clear_bss

check_el:
    /* The firmware starts us at EL2, but the kernel wants to run at EL1 so
     * that EL0 is available for user programs and so the EL1 vector table is
     * the one that matters. Booting already at EL1 is fine too -- skip the
     * drop rather than faulting on the EL2-only registers below. */
    mrs     x1, CurrentEL
    and     x1, x1, #0xC            /* CurrentEL[3:2] holds the level */
    cmp     x1, #(2 << 2)
    b.ne    at_el1

    /* EL1 executes AArch64, not AArch32. */
    mov     x1, #(1 << 31)
    msr     hcr_el2, x1

    /* Without EL1PCTEN|EL1PCEN, EL1 reads of cntpct_el0 and writes to
     * cntp_ctl_el0 trap to EL2 -- the timer exercises depend on this. */
    mrs     x1, cnthctl_el2
    orr     x1, x1, #3
    msr     cnthctl_el2, x1
    msr     cntvoff_el2, xzr

    /* MMU and caches off, EL1 reserved bits set to their required values. */
    ldr     x1, =0x30D00800
    msr     sctlr_el1, x1

    /* Return into EL1h (EL1 using SP_EL1) with interrupts still masked; they
     * are unmasked later, once the vector table is installed. */
    mov     x1, #0x3C5
    msr     spsr_el2, x1
    adr     x1, at_el1
    msr     elr_el2, x1
    eret

at_el1:
    /* SP_EL1 is a different register from the SP_EL2 set above, so the stack
     * has to be established again on this side of the switch. */
    ldr     x1, =_start
    mov     sp, x1

    mov     x0, x20                 /* main(uint64_t dtb_addr) */
    bl      main
    b       hang

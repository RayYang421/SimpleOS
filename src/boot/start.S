#include "mmu.h"

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
     * 0x80000 is free once the bootloader has handed off. Physical for now --
     * it is set again in the upper half once the MMU is on. */
    ldr     x1, =KERNEL_PHYS_BASE
    mov     sp, x1

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

    /* MMU and caches off, EL1 reserved bits set to their required values. Bit
     * 23 among them is SPAN, which leaves PSTATE.PAN clear on exception entry
     * so the kernel can dereference a user pointer directly -- syscalls that
     * take a buffer rely on that. */
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
    ldr     x1, =KERNEL_PHYS_BASE
    mov     sp, x1

    bl      setup_mmu

    /* Everything above ran at its physical address; the kernel is linked for
     * the upper half, so this is where it moves there. The identity mapping is
     * still live, which is the only reason this instruction and the branch
     * target can both be reached. */
    ldr     x1, =kernel_high
    br      x1

kernel_high:
    ldr     x1, =_start             /* now the linked address, not 0x80000 */
    mov     sp, x1

    ldr     x1, =__bss_start
    ldr     x2, =__bss_end
clear_bss:
    cmp     x1, x2
    b.ge    enter_main
    str     xzr, [x1], #8
    b       clear_bss

enter_main:
    mov     x0, x20                 /* main(uint64_t dtb_phys) */
    bl      main
    b       hang

/* --- boot translation tables -------------------------------------------------
 *
 * Two entries' worth of setup gets the kernel running with the MMU on: one PUD
 * covering the first two gigabytes, and a PMD splitting the first of those into
 * 2 MiB blocks. The split is what makes the peripheral window device memory
 * while RAM stays normal memory -- device memory forbids the speculation and
 * gathering that would otherwise reorder MMIO, and an unaligned access to it
 * faults rather than being fixed up.
 *
 * The tables are built at fixed physical addresses because there is no
 * allocator yet, and only immediates and PC-relative branches are used, because
 * a linked address means nothing until the MMU is on. */
setup_mmu:
    ldr     x0, =MAIR_VALUE
    msr     mair_el1, x0

    ldr     x0, =TCR_CONFIG_DEFAULT
    msr     tcr_el1, x0

    /* PGD[0] -> PUD. One PGD entry spans 512 GiB, so entry 0 covers every
     * address the board can produce. */
    mov     x0, #BOOT_PGD_PA
    mov     x1, #BOOT_PUD_PA
    orr     x2, x1, #PD_TABLE
    str     x2, [x0]

    /* PUD[0] -> PMD, describing 0x00000000-0x3fffffff in finer detail. */
    mov     x2, #BOOT_PMD_PA
    orr     x3, x2, #PD_TABLE
    str     x3, [x1]

    /* PUD[1] is the ARM local peripherals (the core timer and its interrupt
     * routing) at 0x40000000, which need no finer split. */
    ldr     x3, =(0x40000000 | PD_ACCESS | PD_ATTR(MAIR_IDX_DEVICE_nGnRnE) | \
                  PD_BLOCK | PD_UXN | PD_PXN)
    str     x3, [x1, #8]

    /* 512 blocks of 2 MiB: RAM up to the peripheral window, device memory from
     * there to the top of the gigabyte. */
    mov     x0, #BOOT_PMD_PA
    mov     x1, xzr                 /* physical address this entry describes */
    ldr     x4, =0x3F000000         /* where the peripherals start */
    ldr     x5, =(PD_ACCESS | PD_ATTR(MAIR_IDX_NORMAL_NOCACHE) | PD_BLOCK | PD_UXN)
    ldr     x6, =(PD_ACCESS | PD_ATTR(MAIR_IDX_DEVICE_nGnRnE) | PD_BLOCK | \
                  PD_UXN | PD_PXN)
    mov     x8, #(1 << 21)          /* one block */
    ldr     x9, =0x40000000         /* end of the region this PMD covers */
fill_pmd:
    cmp     x1, x4
    csel    x7, x5, x6, lo
    orr     x7, x7, x1
    str     x7, [x0], #8
    add     x1, x1, x8
    cmp     x1, x9
    b.lo    fill_pmd

    /* The same tables serve both halves: ttbr0 so the code running right now
     * keeps working across the switch, ttbr1 because that is where the kernel
     * is linked. ttbr0 is handed to user processes later, once nothing needs
     * the identity mapping any more. */
    mov     x0, #BOOT_PGD_PA
    msr     ttbr0_el1, x0
    msr     ttbr1_el1, x0

    /* Every table write has to be visible to the page table walker, which is
     * not the same observer as this core's load/store unit. */
    dsb     ish
    isb

    mrs     x0, sctlr_el1
    orr     x0, x0, #1              /* M: translation on */
    msr     sctlr_el1, x0
    isb

    ret

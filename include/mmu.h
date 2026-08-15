#ifndef MMU_H
#define MMU_H

/* start.S builds the first set of page tables with these definitions, so
 * everything below is written to survive the assembler as well. */
#ifndef __ASSEMBLER__
#include "types.h"
#endif

/* The kernel runs entirely in the upper half of the address space, out of the
 * way of user programs, which get the lower half to themselves.
 *
 * All of physical memory is mapped linearly at this offset, so a physical
 * address and the kernel's view of it differ by a constant -- no kernel access
 * ever has to walk a page table, and page_alloc can keep handing back pointers
 * that are usable as-is. */
#define KERNEL_VA_BASE  0xffff000000000000UL
#define PA_MASK         0x0000ffffffffffffUL

/* Where the firmware loads a raw AArch64 image, and so where the kernel's own
 * linked address maps back to. */
#define KERNEL_PHYS_BASE 0x80000UL

#define PA(va) ((uint64_t)(va) & PA_MASK)
#define VA(pa) ((void *)((uint64_t)(pa) | KERNEL_VA_BASE))

/* --- translation table geometry ---
 *
 * Four levels of 512 entries over 4 KiB granules: bits [47:39] select the PGD
 * entry, [38:30] the PUD entry, [30:21] the PMD entry and [20:12] the PTE. */
#define PGD_SHIFT       39
#define PUD_SHIFT       30
#define PMD_SHIFT       21
#define PTE_SHIFT       12
#define TABLE_ENTRIES   512
#define TABLE_MASK      (TABLE_ENTRIES - 1)

#define TABLE_INDEX(va, shift) (((va) >> (shift)) & TABLE_MASK)

/* --- descriptor format --- */
#define PD_INVALID      0UL
#define PD_BLOCK        0b01UL      /* PUD/PMD entry naming memory directly */
#define PD_TABLE        0b11UL      /* entry naming the next level down */
#define PD_PAGE         0b11UL      /* PTE entry naming a 4 KiB page */
#define PD_TYPE_MASK    0b11UL

#define PD_ACCESS       (1UL << 10) /* clear means "generate a fault on use" */
#define PD_USER         (1UL << 6)  /* AP[1]: reachable from EL0 as well */
#define PD_RDONLY       (1UL << 7)  /* AP[2]: read-only at both levels */
#define PD_PXN          (1UL << 53) /* not executable at EL1 */
#define PD_UXN          (1UL << 54) /* not executable at EL0 */

/* Software bits, ignored by the MMU.
 *
 * COW marks a mapping that is read-only only because it is shared after a
 * fork, so a write fault can tell "copy this" from "this really is read-only".
 * NOFREE marks a frame the address space borrows rather than owns -- the page
 * of kernel code that runs at EL0 -- so tearing the address space down leaves
 * it alone. */
#define PD_COW          (1UL << 55)
#define PD_NOFREE       (1UL << 56)

/* The address field of a descriptor: bits [47:12]. */
#define PD_ADDR_MASK    0x0000fffffffff000UL

/* --- memory attributes ---
 *
 * The policies live in MAIR_EL1 and descriptors carry a three-bit index into
 * it, so a page's caching behaviour is named rather than spelled out. */
#define MAIR_DEVICE_nGnRnE      0b00000000
#define MAIR_NORMAL_NOCACHE     0b01000100
#define MAIR_IDX_DEVICE_nGnRnE  0
#define MAIR_IDX_NORMAL_NOCACHE 1
#define MAIR_VALUE  ((MAIR_DEVICE_nGnRnE  << (8 * MAIR_IDX_DEVICE_nGnRnE)) | \
                     (MAIR_NORMAL_NOCACHE << (8 * MAIR_IDX_NORMAL_NOCACHE)))

#define PD_ATTR(idx)    ((idx) << 2)

/* --- TCR_EL1 --- */
#define TCR_CONFIG_REGION_48BIT (((64 - 48) << 0) | ((64 - 48) << 16))
#define TCR_CONFIG_4KB          ((0b00UL << 14) | (0b10UL << 30))
#define TCR_CONFIG_DEFAULT      (TCR_CONFIG_REGION_48BIT | TCR_CONFIG_4KB)

/* Physical addresses of the tables start.S builds before turning the MMU on.
 * They sit below the kernel image, in memory the firmware has finished with,
 * and are fixed rather than allocated because there is no allocator yet.
 * meminit reserves them. */
#define BOOT_PGD_PA     0x1000UL
#define BOOT_PUD_PA     0x2000UL
#define BOOT_PMD_PA     0x3000UL
#define BOOT_TABLES_END 0x4000UL

#ifndef __ASSEMBLER__

/* Drops the identity mapping the kernel booted through. Everything runs at its
 * link address in the upper half by then, so the low half belongs to user
 * processes from here on. */
void mmu_drop_identity_map(void);

/* The empty table ttbr0_el1 gets while a thread with no address space of its
 * own is running, so a stray access to a low address faults instead of landing
 * in whatever process ran last. */
uint64_t mmu_empty_pgd(void);

static inline void mmu_set_user_table(uint64_t pgd_pa) {
    __asm__ volatile(
        "dsb ish\n"             /* the table writes must land before it is used */
        "msr ttbr0_el1, %0\n"
        "tlbi vmalle1is\n"      /* nothing cached from the old address space */
        "dsb ish\n"
        "isb\n"                 /* and nothing already in the pipeline */
        :: "r"(pgd_pa) : "memory");
}

/* After changing a live descriptor: the walk that faulted may have cached the
 * old entry. */
static inline void mmu_flush_tlb(void) {
    __asm__ volatile("dsb ish\n tlbi vmalle1is\n dsb ish\n isb\n" ::: "memory");
}

#endif /* __ASSEMBLER__ */

#endif

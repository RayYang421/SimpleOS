#include "vm.h"
#include "mmu.h"
#include "page.h"
#include "mm.h"
#include "sched.h"
#include "uart.h"
#include "irq.h"
#include "string.h"

/* Data and instruction abort syndromes. Bits [5:2] of the ISS name the kind of
 * fault and bits [1:0] the level it happened at; bit 6 (data aborts only) says
 * whether the access was a write. */
#define FAULT_TRANSLATION(esr) (((esr) & 0x3C) == 0x04)
#define FAULT_PERMISSION(esr)  (((esr) & 0x3C) == 0x0C)
#define FAULT_IS_WRITE(esr)    (((esr) >> 6) & 1)

#define EC_INSN_ABORT_LOWER  0x20
#define EC_INSN_ABORT_SAME   0x21
#define EC_DATA_ABORT_LOWER  0x24
#define EC_DATA_ABORT_SAME   0x25

/* ttbr0_el1 while a thread with no address space of its own runs. A stray
 * access to a low address then faults, instead of quietly reading whatever
 * process happened to run last. */
static uint64_t empty_pgd;
static uint64_t installed_pgd;

/* Attributes for a leaf entry. Everything a user process can reach is normal
 * memory that EL1 must never execute -- a kernel that jumps into a user buffer
 * is a kernel that has already been taken over. */
static uint64_t pte_attr(int prot, int cow, int nofree) {
    uint64_t attr = PD_PAGE | PD_ACCESS | PD_USER | PD_PXN |
                    PD_ATTR(MAIR_IDX_NORMAL_NOCACHE);

    if (!(prot & PROT_WRITE) || cow) attr |= PD_RDONLY;
    if (!(prot & PROT_EXEC))         attr |= PD_UXN;
    if (cow)                         attr |= PD_COW;
    if (nofree)                      attr |= PD_NOFREE;

    return attr;
}

/* Walks down to the entry describing va, creating the tables on the way when
 * asked to. The three upper levels hold no permissions of their own -- they
 * are left wide open and the leaf decides -- so that changing a page's
 * protection never means revisiting the levels above it. */
static uint64_t *walk(uint64_t pgd, uint64_t va, int alloc) {
    static const int shift[3] = { PGD_SHIFT, PUD_SHIFT, PMD_SHIFT };

    if (pgd == 0) return 0;
    uint64_t *table = (uint64_t *)VA(pgd);

    for (int level = 0; level < 3; level++) {
        uint64_t *entry = &table[TABLE_INDEX(va, shift[level])];

        if ((*entry & PD_TYPE_MASK) == PD_TABLE) {
            table = (uint64_t *)VA(*entry & PD_ADDR_MASK);
            continue;
        }

        if (!alloc) return 0;

        void *next = page_alloc(0);
        if (next == 0) return 0;

        memset(next, 0, PAGE_SIZE);
        *entry = PA(next) | PD_TABLE;
        table  = (uint64_t *)next;
    }

    return &table[TABLE_INDEX(va, PTE_SHIFT)];
}

int mappages(uint64_t pgd, uint64_t va, uint64_t size, uint64_t pa, int prot,
             int nofree) {
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t *pte = walk(pgd, va + off, 1);
        if (pte == 0) return -1;
        *pte = ((pa + off) & PD_ADDR_MASK) | pte_attr(prot, 0, nofree);
    }

    mmu_flush_tlb();
    return 0;
}

uint64_t vm_va_to_pa(uint64_t pgd, uint64_t va) {
    uint64_t *pte = walk(pgd, va & ~(PAGE_SIZE - 1), 0);
    if (pte == 0 || (*pte & PD_TYPE_MASK) != PD_PAGE) return 0;
    return (*pte & PD_ADDR_MASK) | (va & (PAGE_SIZE - 1));
}

uint64_t vm_new_pgd(void) {
    void *pgd = page_alloc(0);
    if (pgd == 0) return 0;

    memset(pgd, 0, PAGE_SIZE);
    return PA(pgd);
}

uint64_t mmu_empty_pgd(void) {
    if (empty_pgd == 0) empty_pgd = vm_new_pgd();
    return empty_pgd;
}

void mmu_drop_identity_map(void) {
    /* The boot tables serve both halves through the same entry, so the low half
     * cannot be unmapped -- it is given away instead, to a table with nothing
     * in it. */
    vm_switch(0);
}

void vm_switch(uint64_t pgd) {
    if (pgd == 0) pgd = mmu_empty_pgd();
    if (pgd == installed_pgd) return;

    installed_pgd = pgd;
    mmu_set_user_table(pgd);
}

/* --- regions --------------------------------------------------------------- */

static uint64_t page_down(uint64_t v) { return v & ~(PAGE_SIZE - 1); }
static uint64_t page_up(uint64_t v)   { return (v + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1); }

/* The page of kernel code that has to be executable at EL0: the signal
 * trampoline and the exception demo, given a section of their own by the
 * linker script so nothing else shares their page. */
extern char __user_page_start[];
extern char __user_page_end[];

uint64_t vm_shared_va(void *kernel_addr) {
    return USER_SHARED_VA +
           ((uint64_t)(uintptr_t)kernel_addr - (uint64_t)(uintptr_t)__user_page_start);
}

int vm_new_address_space(struct thread *t, const char *image, uint64_t size) {
    vm_destroy(t);

    t->pgd = vm_new_pgd();
    if (t->pgd == 0) return -1;

    uint64_t shared = (uint64_t)(uintptr_t)__user_page_end -
                      (uint64_t)(uintptr_t)__user_page_start;

    /* Borrowed from the kernel image rather than allocated, so it is mapped
     * outright: there is nothing to demand-load and nothing to free. */
    if (vm_add_area(t, USER_SHARED_VA, shared, PROT_READ | PROT_EXEC, VM_PHYS,
                    0, 0, PA(__user_page_start)) != 0) return -1;

    if (vm_add_area(t, USER_STACK_BASE, USER_STACK_SIZE,
                    PROT_READ | PROT_WRITE, MAP_ANONYMOUS, 0, 0, 0) != 0) return -1;

    if (image) {
        /* Linked at 0 and loaded at 0, so nothing in it has to be relocatable.
         * The pages are copied out of the initramfs as they are first touched,
         * which is why the region remembers where the file is. */
        if (vm_add_area(t, USER_TEXT_VA, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                        0, image, size, 0) != 0) return -1;
    }

    return 0;
}

struct vm_area *vm_find(struct thread *t, uint64_t va) {
    if (t == 0) return 0;

    for (struct vm_area *a = t->vma; a; a = a->next) {
        if (va >= a->start && va < a->end) return a;
    }
    return 0;
}

static int overlaps(struct thread *t, uint64_t start, uint64_t end) {
    for (struct vm_area *a = t->vma; a; a = a->next) {
        if (start < a->end && a->start < end) return 1;
    }
    return 0;
}

int vm_add_area(struct thread *t, uint64_t va, uint64_t size, int prot,
                int flags, const char *src, uint64_t src_size, uint64_t pa) {
    uint64_t start = page_down(va);
    uint64_t end   = page_up(va + size);

    if (t == 0 || end <= start) return -1;
    if (overlaps(t, start, end)) return -1;

    struct vm_area *a = kmalloc(sizeof(struct vm_area));
    if (a == 0) return -1;

    a->start    = start;
    a->end      = end;
    a->prot     = prot;
    a->flags    = flags;
    a->src      = src;
    a->src_size = src_size;
    a->pa       = pa;

    a->next = t->vma;
    t->vma  = a;
    return 0;
}

/* --- tearing an address space down ------------------------------------------ */

static void free_table(uint64_t table_pa, int level) {
    uint64_t *table = (uint64_t *)VA(table_pa);

    for (int i = 0; i < TABLE_ENTRIES; i++) {
        uint64_t entry = table[i];
        if ((entry & PD_TYPE_MASK) == PD_INVALID) continue;

        if (level < 3) {
            free_table(entry & PD_ADDR_MASK, level + 1);
        } else if (!(entry & PD_NOFREE)) {
            /* Shared after a fork until the last address space lets go. */
            page_ref_dec(entry & PD_ADDR_MASK);
        }
    }

    page_free(VA(table_pa));
}

void vm_destroy(struct thread *t) {
    if (t == 0) return;

    struct vm_area *a = t->vma;
    t->vma = 0;

    while (a) {
        struct vm_area *next = a->next;
        kfree(a);
        a = next;
    }

    if (t->pgd) {
        /* The table being freed may be the one ttbr0_el1 still points at. */
        if (installed_pgd == t->pgd) vm_switch(0);
        free_table(t->pgd, 0);
        t->pgd = 0;
    }
}

/* --- fork ------------------------------------------------------------------- */

/* Shares one page with the child. Both sides come out read-only, so whichever
 * writes first takes a permission fault and gets a private copy -- including
 * the parent, whose page was writable a moment ago. */
static int share_page(struct thread *child, struct thread *parent,
                      struct vm_area *area, uint64_t va) {
    uint64_t *ppte = walk(parent->pgd, va, 0);
    if (ppte == 0 || (*ppte & PD_TYPE_MASK) != PD_PAGE) return 0;  /* not faulted in yet */

    uint64_t *cpte = walk(child->pgd, va, 1);
    if (cpte == 0) return -1;

    uint64_t pa     = *ppte & PD_ADDR_MASK;
    int      nofree = (*ppte & PD_NOFREE) != 0;

    if (nofree) {
        /* Memory the kernel lends both of them; nothing to copy, ever. */
        *cpte = pa | pte_attr(area->prot, 0, 1);
        return 0;
    }

    /* A region that was never writable needs no copy-on-write bookkeeping:
     * sharing it read-only is the end of the story. */
    int cow = (area->prot & PROT_WRITE) != 0;

    *ppte = pa | pte_attr(area->prot, cow, 0);
    *cpte = pa | pte_attr(area->prot, cow, 0);
    page_ref_inc(pa);
    return 0;
}

int vm_fork(struct thread *child, struct thread *parent) {
    child->pgd = vm_new_pgd();
    if (child->pgd == 0) return -1;

    child->vma = 0;

    for (struct vm_area *a = parent->vma; a; a = a->next) {
        if (vm_add_area(child, a->start, a->end - a->start, a->prot, a->flags,
                        a->src, a->src_size, a->pa) != 0)
            return -1;

        for (uint64_t va = a->start; va < a->end; va += PAGE_SIZE) {
            if (share_page(child, parent, a, va) != 0) return -1;
        }
    }

    /* The parent's own entries just became read-only. */
    mmu_flush_tlb();
    return 0;
}

/* --- faults ----------------------------------------------------------------- */

static void segfault(uint64_t far) {
    struct thread *t = current();

    uart_puts("[Segmentation fault]: Kill Process\n");
    uart_puts("  pid ");
    uart_dec(t ? t->pid : -1);
    uart_puts(" touched ");
    uart_hex(far);
    uart_puts("\n");

    thread_exit(-1);            /* does not return */
}

/* First touch of a page: find something to put behind it. */
static int demand_page(struct thread *t, struct vm_area *area, uint64_t va) {
    uart_puts("[Translation fault]: ");
    uart_hex(va);
    uart_puts("\n");

    if (area->pa) {
        /* Backed by memory the kernel already owns. */
        return mappages(t->pgd, va, PAGE_SIZE,
                        area->pa + (va - area->start), area->prot, 1);
    }

    void *frame = page_alloc(0);
    if (frame == 0) return -1;

    memset(frame, 0, PAGE_SIZE);

    /* A file-backed region copies its slice of the image in; past the end of
     * the file the page stays zero, which is what gives a program its .bss. */
    if (area->src) {
        uint64_t off = va - area->start;

        if (off < area->src_size) {
            uint64_t n = area->src_size - off;
            if (n > PAGE_SIZE) n = PAGE_SIZE;
            memcpy(frame, area->src + off, n);
        }
    }

    return mappages(t->pgd, va, PAGE_SIZE, PA(frame), area->prot, 0);
}

/* A write to a page that fork left shared. */
static int copy_page(struct thread *t, struct vm_area *area, uint64_t va) {
    uint64_t *pte = walk(t->pgd, va, 0);
    if (pte == 0 || (*pte & PD_TYPE_MASK) != PD_PAGE) return -1;

    uint64_t pa = *pte & PD_ADDR_MASK;

    if (page_ref_get(pa) <= 1) {
        /* Everyone else has already gone their own way: no copy needed, just
         * hand the write permission back. */
        *pte = pa | pte_attr(area->prot, 0, 0);
        mmu_flush_tlb();
        return 0;
    }

    void *fresh = page_alloc(0);
    if (fresh == 0) return -1;

    memcpy(fresh, VA(pa), PAGE_SIZE);
    page_ref_dec(pa);

    *pte = PA(fresh) | pte_attr(area->prot, 0, 0);
    mmu_flush_tlb();
    return 0;
}

int vm_fault(uint64_t esr, uint64_t far) {
    struct thread *t = current();
    uint64_t ec = esr >> 26;

    if (ec != EC_DATA_ABORT_LOWER && ec != EC_DATA_ABORT_SAME &&
        ec != EC_INSN_ABORT_LOWER && ec != EC_INSN_ABORT_SAME) return -1;

    /* Only the lower half is anyone's user address space; a fault above it is a
     * kernel bug and belongs to the panic path. */
    if (t == 0 || t->pgd == 0 || (far & KERNEL_VA_BASE)) return -1;

    uint64_t va = page_down(far);
    struct vm_area *area = vm_find(t, va);

    if (area == 0 || area->prot == PROT_NONE) segfault(far);

    /* An instruction abort is a fetch, never a write. */
    int is_data  = (ec == EC_DATA_ABORT_LOWER || ec == EC_DATA_ABORT_SAME);
    int is_write = is_data && FAULT_IS_WRITE(esr);

    if (FAULT_TRANSLATION(esr)) {
        if (demand_page(t, area, va) != 0) {
            uart_puts("[page fault]: out of memory\n");
            segfault(far);
        }
        return 0;
    }

    if (FAULT_PERMISSION(esr)) {
        uint64_t *pte = walk(t->pgd, va, 0);

        /* A write to a region that is genuinely read-only, rather than one the
         * fork made read-only for a moment. */
        if (!is_write || pte == 0 || !(*pte & PD_COW) ||
            !(area->prot & PROT_WRITE)) segfault(far);

        if (copy_page(t, area, va) != 0) {
            uart_puts("[page fault]: out of memory\n");
            segfault(far);
        }
        return 0;
    }

    return -1;
}

/* --- mmap ------------------------------------------------------------------- */

/* Somewhere the process is not already using. On a collision the search
 * resumes past the region that got in the way, so it takes one step per region
 * rather than one per page. */
static uint64_t find_gap(struct thread *t, uint64_t len) {
    uint64_t va = USER_MMAP_BASE;

    while (va + len <= USER_SHARED_VA) {
        uint64_t next = 0;

        for (struct vm_area *a = t->vma; a; a = a->next) {
            if (va < a->end && a->start < va + len && a->end > next) next = a->end;
        }

        if (next == 0) return va;
        va = next;
    }

    return 0;
}

void *vm_mmap(uint64_t addr, uint64_t len, int prot, int flags) {
    struct thread *t = current();

    if (t == 0 || t->pgd == 0 || len == 0) return (void *)-1;

    len = page_up(len);

    /* An address that is page-aligned and free is used as asked; anything else
     * is only a hint, and the kernel picks. */
    uint64_t start = page_down(addr);

    if (addr == 0 || (addr & (PAGE_SIZE - 1)) || overlaps(t, start, start + len))
        start = find_gap(t, len);

    if (start == 0) return (void *)-1;

    if (vm_add_area(t, start, len, prot, flags, 0, 0, 0) != 0) return (void *)-1;

    /* MAP_POPULATE asks for the frames now rather than on first touch. */
    if (flags & MAP_POPULATE) {
        struct vm_area *area = vm_find(t, start);

        for (uint64_t va = start; va < start + len; va += PAGE_SIZE) {
            void *frame = page_alloc(0);
            if (frame == 0) return (void *)-1;

            memset(frame, 0, PAGE_SIZE);
            if (mappages(t->pgd, va, PAGE_SIZE, PA(frame), area->prot, 0) != 0)
                return (void *)-1;
        }
    }

    return (void *)start;
}

/* --- reporting -------------------------------------------------------------- */

/* Held together against preemption: the process being reported on is usually
 * still running, and could exit and have its regions freed part-way through. */
void vm_report(struct thread *t) {
    if (t == 0 || t->pgd == 0) {
        uart_puts("no user address space (try vm <pid> while a program runs)\n");
        return;
    }

    preempt_disable();

    uart_puts("pid ");
    uart_dec(t->pid);
    uart_puts("  pgd: ");
    uart_hex(t->pgd);
    uart_puts("\nstart             end               prot  mapped\n");

    for (struct vm_area *a = t->vma; a; a = a->next) {
        uint64_t mapped = 0;
        for (uint64_t va = a->start; va < a->end; va += PAGE_SIZE) {
            if (vm_va_to_pa(t->pgd, va)) mapped++;
        }

        uart_hex(a->start);
        uart_puts("  ");
        uart_hex(a->end);
        uart_puts("  ");
        uart_puts((a->prot & PROT_READ)  ? "r" : "-");
        uart_puts((a->prot & PROT_WRITE) ? "w" : "-");
        uart_puts((a->prot & PROT_EXEC)  ? "x" : "-");
        uart_puts("   ");
        uart_dec(mapped);
        uart_puts("/");
        uart_dec((a->end - a->start) >> PAGE_SHIFT);
        uart_puts(" page(s)\n");
    }

    preempt_enable();
}

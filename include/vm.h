#ifndef VM_H
#define VM_H

#include "types.h"
#include "exception.h"

struct thread;

/* --- the user address space ---
 *
 * Every process gets a PGD of its own, so the same virtual address means
 * something different in each of them. Nothing is mapped up front: a process
 * starts with an empty page table and a list of the regions it is entitled to,
 * and the page fault handler fills in a frame the first time each page is
 * actually touched.
 *
 * The layout a program is loaded into:
 *
 *   0x0000000000000000  the program image, demand-loaded from the initramfs
 *   ...                 anything mmap has handed out
 *   0x0000ffffffffa000  one page of kernel code that runs at EL0
 *   0x0000ffffffffb000  the stack, four pages, growing down from the top
 */
#define USER_TEXT_VA     0x0000000000000000UL
#define USER_SHARED_VA   0x0000ffffffff0000UL
#define USER_STACK_BASE  0x0000ffffffffb000UL
#define USER_STACK_TOP   0x0000fffffffff000UL
#define USER_STACK_SIZE  (USER_STACK_TOP - USER_STACK_BASE)

/* Where mmap starts looking when the caller does not care where a region goes.
 * Well clear of both the program image and the stack. */
#define USER_MMAP_BASE   0x0000000010000000UL

/* The protection bits mmap takes, and the lab's values for them. */
#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
#define PROT_EXEC   4

/* Linux's values, since a user program written against the usual headers
 * should work here unchanged. */
#define MAP_ANONYMOUS   0x20
#define MAP_POPULATE    0x8000

/* Set on regions the kernel backs with memory it already owns -- the shared
 * EL0 page -- so tearing the address space down does not free it. */
#define VM_PHYS         0x40000000

struct vm_area {
    uint64_t start;         /* page-aligned bounds in the user address space */
    uint64_t end;
    int      prot;
    int      flags;

    /* Where the contents come from the first time a page is touched. A region
     * with a source demand-loads from it (the program image, in the
     * initramfs); one with a fixed physical base maps that memory directly;
     * anything else is anonymous and faults in zeroed frames. */
    const char *src;
    uint64_t    src_size;
    uint64_t    pa;

    struct vm_area *next;
};

/* An empty address space: one zeroed PGD frame, returned as a physical address
 * because that is what ttbr0_el1 takes. 0 if memory ran out. */
uint64_t vm_new_pgd(void);

/* Replaces a thread's address space with a fresh one holding the shared EL0
 * page, a stack, and -- when an image is given -- the program itself, mapped
 * at address 0 and demand-loaded from the initramfs. Returns 0 on success. */
int vm_new_address_space(struct thread *t, const char *image, uint64_t size);

/* Where a kernel address inside the shared EL0 page appears in user space. */
uint64_t vm_shared_va(void *kernel_addr);

/* Releases every frame an address space owns, its page tables, and its region
 * list. Safe on a thread that never had one. */
void vm_destroy(struct thread *t);

/* Records that a range of user addresses is usable, without mapping anything.
 * Returns 0, or -1 if it would overlap a region that already exists. */
int vm_add_area(struct thread *t, uint64_t va, uint64_t size, int prot,
                int flags, const char *src, uint64_t src_size, uint64_t pa);

struct vm_area *vm_find(struct thread *t, uint64_t va);

/* Fills in page table entries for size bytes at va, allocating the levels in
 * between as it goes. Returns 0 on success. */
int mappages(uint64_t pgd, uint64_t va, uint64_t size, uint64_t pa, int prot,
             int nofree);

/* Gives the child the parent's regions and page tables, sharing every mapped
 * frame read-only so the first write to one copies it. */
int vm_fork(struct thread *child, struct thread *parent);

/* Points ttbr0_el1 at an address space, or at an empty one when a thread has
 * none. Skips the work when the table is already installed. */
void vm_switch(uint64_t pgd);

/* Handles a fault against the current thread's address space: demand paging
 * for a page that has never been touched, copy-on-write for a write to a
 * shared one. Returns 0 if the faulting instruction can be retried, -1 if the
 * fault does not belong to a user address space at all. A fault that is a real
 * access violation terminates the process rather than returning. */
int vm_fault(uint64_t esr, uint64_t far);

/* Physical address backing a user address, or 0 if nothing is mapped there. */
uint64_t vm_va_to_pa(uint64_t pgd, uint64_t va);

/* mmap, as the syscall sees it. Returns the address of the new region, or -1
 * cast to a pointer. */
void *vm_mmap(uint64_t addr, uint64_t len, int prot, int flags);

void vm_report(struct thread *t);

#endif

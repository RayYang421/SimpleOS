#include "mbox.h"
#include "mmio.h"
#include "irq.h"

#define MBOX_BASE    (MMIO_BASE + 0x0000B880)
#define MBOX_READ    (MBOX_BASE + 0x00)
#define MBOX_STATUS  (MBOX_BASE + 0x18)
#define MBOX_WRITE   (MBOX_BASE + 0x20)

#define MBOX_EMPTY   0x40000000u
#define MBOX_FULL    0x80000000u

#define MBOX_REQUEST 0x00000000u
#define MBOX_RESPONSE_OK 0x80000000u

#define TAG_GET_BOARD_REVISION 0x00010002u
#define TAG_GET_ARM_MEMORY     0x00010005u
#define TAG_LAST               0x00000000u

/* Shared with the VideoCore, so it must be 16-byte aligned for the channel
 * bits to fit in the low nibble of the address. */
static volatile uint32_t buffer[36] __attribute__((aligned(16)));

int mbox_call(unsigned char channel, unsigned int *mbox) {
    uint64_t addr = (uint64_t)(uintptr_t)mbox;

    /* The mailbox takes a 32-bit address with the channel in the low bits, so
     * a buffer above 4 GiB or misaligned cannot be passed at all. */
    if (addr & 0xF) return 0;
    if (addr >> 32) return 0;

    uint32_t message = ((uint32_t)addr & ~0xFu) | (channel & 0xF);

    uint64_t daif = irq_disable_save();

    while (mmio_read(MBOX_STATUS) & MBOX_FULL) { }
    mmio_write(MBOX_WRITE, message);

    for (;;) {
        while (mmio_read(MBOX_STATUS) & MBOX_EMPTY) { }
        if (mmio_read(MBOX_READ) == message) break;
    }

    irq_restore(daif);
    return mbox[1] == MBOX_RESPONSE_OK;
}

static int simple_tag(uint32_t tag, uint32_t *out, int nwords) {
    buffer[0] = (uint32_t)((6 + nwords) * 4);
    buffer[1] = MBOX_REQUEST;
    buffer[2] = tag;
    buffer[3] = (uint32_t)(nwords * 4);
    buffer[4] = 0;
    for (int i = 0; i < nwords; i++) buffer[5 + i] = 0;
    buffer[5 + nwords] = TAG_LAST;

    if (!mbox_call(8, (unsigned int *)buffer)) return -1;

    for (int i = 0; i < nwords; i++) out[i] = buffer[5 + i];
    return 0;
}

int mbox_board_revision(uint32_t *revision) {
    return simple_tag(TAG_GET_BOARD_REVISION, revision, 1);
}

int mbox_arm_memory(uint32_t *base, uint32_t *size) {
    uint32_t out[2];
    if (simple_tag(TAG_GET_ARM_MEMORY, out, 2) != 0) return -1;
    *base = out[0];
    *size = out[1];
    return 0;
}

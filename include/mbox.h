#ifndef MBOX_H
#define MBOX_H

#include "types.h"

/* VideoCore mailbox. The buffer must be 16-byte aligned, because the low four
 * bits of the address carry the channel number. */
int mbox_call(unsigned char channel, unsigned int *mbox);

/* Board revision and the ARM memory window, read through the mailbox. Return 0
 * on success. */
int mbox_board_revision(uint32_t *revision);
int mbox_arm_memory(uint32_t *base, uint32_t *size);

#endif

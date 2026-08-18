#ifndef CPIO_H
#define CPIO_H

#include "types.h"

/* Address the initramfs was found at, either from the devicetree or from the
 * platform default. Set once during boot before the shell starts. */
void  cpio_set_base(void *addr);
void *cpio_get_base(void);

/* 1 if the base address holds something that actually parses as a newc
 * archive, 0 otherwise -- lets the shell explain itself instead of faulting. */
int cpio_valid(void);

/* Total bytes the archive occupies, including the trailer. 0 if there is no
 * archive. Used to reserve the initramfs when the devicetree does not say
 * where it ends. */
size_t cpio_size(void);

void cpio_ls(void);

/* Walks the archive one entry at a time. Start at cpio_get_base(); the return
 * value is the next entry, or NULL at the end. Any out parameter may be null.
 * The mode is the stat mode: CPIO_MODE_DIR distinguishes a directory from an
 * empty file, which the name alone cannot. */
#define CPIO_MODE_FMT 0170000u
#define CPIO_MODE_DIR 0040000u

const char *cpio_next(const char *ptr, const char **name, const char **data,
                      uint32_t *size, uint32_t *mode);

/* Returns 0 when path is found, filling in the data pointer and size,
 * and -1 otherwise. */
int cpio_lookup(const char *path, const char **data, size_t *size);

/* Returns 0 on success, -1 if the file does not exist. */
int cpio_cat(const char *path);

#endif

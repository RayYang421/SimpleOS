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

void cpio_ls(void);

/* Returns 0 when path is found, filling in the data pointer and size,
 * and -1 otherwise. */
int cpio_lookup(const char *path, const char **data, size_t *size);

/* Returns 0 on success, -1 if the file does not exist. */
int cpio_cat(const char *path);

#endif

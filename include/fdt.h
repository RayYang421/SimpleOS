#ifndef FDT_H
#define FDT_H

#include "types.h"

/* Invoked once per property in the flattened devicetree.
 *   node - name of the node the property belongs to ("" for the root node)
 *   prop - property name
 *   val  - property value, still in the devicetree's big-endian byte order
 *   len  - length of val in bytes
 *   arg  - opaque pointer handed through from fdt_traverse */
typedef void (*fdt_callback)(const char *node, const char *prop,
                             const void *val, uint32_t len, void *arg);

/* Records the devicetree the bootloader/firmware passed in x0 and, if it is a
 * valid blob, locates the initramfs through it. Safe to call with a bogus
 * address: it validates the header before dereferencing anything else. */
void fdt_init(uint64_t dtb_addr);

/* Address the kernel was booted with, or 0 if no valid devicetree was found. */
uint64_t fdt_get_base(void);

/* Walks the struct block, invoking cb for every property.
 * Returns 0 on success, -1 if the blob is missing or malformed. */
int fdt_traverse(fdt_callback cb, void *arg);

/* Reads a big-endian 32-bit cell -- every integer in a devicetree is stored
 * big-endian regardless of the CPU's byte order. */
uint32_t fdt_be32(const void *p);

/* Total size of the blob, so it can be reserved. 0 if there is no devicetree. */
uint64_t fdt_get_totalsize(void);

/* 1 if node names a /memory node ("memory" or "memory@..."). */
int fdt_is_memory_node(const char *node);

/* Usable RAM from the /memory node. Returns 0 on success, -1 if the
 * devicetree does not describe it. */
int fdt_get_memory(uint64_t *base, uint64_t *size);

/* Where the loader put the initial ramdisk, from /chosen. Returns 0 on
 * success, -1 if the properties are absent. */
int fdt_get_initrd(uint64_t *start, uint64_t *end);

#endif

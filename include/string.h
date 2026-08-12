#ifndef STRING_H
#define STRING_H

#include "types.h"

int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
size_t strlen(const char *s);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memset(void *dst, int c, size_t n);

/* Parses a non-negative integer, accepting a 0x prefix for hex. Returns 0 and
 * leaves *out untouched if the string is not a well-formed number. */
int parse_uint(const char *s, uint64_t *out);

#endif

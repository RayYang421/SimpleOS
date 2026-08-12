#ifndef TYPES_H
#define TYPES_H

/* Built freestanding with -nostdinc, so the fixed-width types the rest of the
 * kernel needs are spelled out here rather than pulled from libc. */

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long       uint64_t;

typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long         int64_t;

typedef unsigned long       size_t;
typedef unsigned long       uintptr_t;

#define NULL ((void *)0)

#endif

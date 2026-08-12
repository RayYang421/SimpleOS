#include "string.h"

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

size_t strlen(const char *s) {
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int parse_uint(const char *s, uint64_t *out) {
    uint64_t v = 0;
    int base = 10;

    if (*s == '\0') return 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
        if (*s == '\0') return 0;
    }

    for (; *s; s++) {
        unsigned int digit;
        if (*s >= '0' && *s <= '9')                   digit = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f') digit = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F') digit = *s - 'A' + 10;
        else return 0;
        v = v * (uint64_t)base + digit;
    }

    *out = v;
    return 1;
}

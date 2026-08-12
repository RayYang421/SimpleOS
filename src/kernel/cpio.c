#include "cpio.h"
#include "uart.h"
#include "string.h"

/* New ASCII Format ("newc", see cpio(5)). Every field is ASCII hex, zero
 * padded, with no separators, so the header is exactly 110 bytes. */
struct cpio_newc_header {
    char magic[6];
    char ino[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];
    char devmajor[8];
    char devminor[8];
    char rdevmajor[8];
    char rdevminor[8];
    char namesize[8];
    char check[8];
};

#define CPIO_HEADER_SIZE  110
#define CPIO_TRAILER      "TRAILER!!!"

/* Sanity bounds. An entry larger than this, or a name longer than this, means
 * we are reading garbage rather than an archive, so stop instead of walking
 * off into unmapped memory. */
#define CPIO_MAX_FILESIZE 0x10000000u   /* 256 MiB */
#define CPIO_MAX_NAMESIZE 4096u

static const char *cpio_base;

void cpio_set_base(void *addr) { cpio_base = (const char *)addr; }
void *cpio_get_base(void)      { return (void *)cpio_base; }

/* Parses an 8-digit ASCII hex field. Returns 0 on a non-hex character, which
 * the callers treat as a malformed header. */
static int parse_hex8(const char *p, uint32_t *out) {
    uint32_t r = 0;

    for (int i = 0; i < 8; i++) {
        char c = p[i];
        uint32_t v;
        if      (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else return 0;
        r = (r << 4) | v;
    }

    *out = r;
    return 1;
}

static uint32_t align4(uint32_t x) { return (x + 3u) & ~3u; }

/* Decodes the entry at ptr. On success fills the out params and returns a
 * pointer to the next entry; returns NULL at the trailer or on any malformed
 * field, which ends iteration. */
static const char *cpio_next(const char *ptr, const char **name,
                             const char **data, uint32_t *size) {
    const struct cpio_newc_header *h = (const struct cpio_newc_header *)ptr;

    /* "070701" is newc, "070702" is newc with a checksum field; the layout is
     * identical, so both parse the same way. */
    if (strncmp(h->magic, "070701", 6) != 0 && strncmp(h->magic, "070702", 6) != 0)
        return 0;

    uint32_t namesize, filesize;
    if (!parse_hex8(h->namesize, &namesize)) return 0;
    if (!parse_hex8(h->filesize, &filesize)) return 0;
    if (namesize == 0 || namesize > CPIO_MAX_NAMESIZE) return 0;
    if (filesize > CPIO_MAX_FILESIZE) return 0;

    const char *entry_name = ptr + CPIO_HEADER_SIZE;

    /* The name is NUL-terminated within namesize bytes; if it is not, the
     * header is lying and every later offset would be wrong. */
    if (entry_name[namesize - 1] != '\0') return 0;

    /* Both the name and the data are padded out to a 4-byte boundary. */
    const char *file_data = ptr + align4(CPIO_HEADER_SIZE + namesize);
    const char *next      = file_data + align4(filesize);

    if (strcmp(entry_name, CPIO_TRAILER) == 0) return 0;

    if (name) *name = entry_name;
    if (data) *data = file_data;
    if (size) *size = filesize;
    return next;
}

int cpio_valid(void) {
    if (cpio_base == 0) return 0;
    /* An archive of only the trailer is still a valid archive, so accept a
     * recognised magic rather than requiring at least one real entry. */
    return strncmp(cpio_base, "070701", 6) == 0 ||
           strncmp(cpio_base, "070702", 6) == 0;
}

void cpio_ls(void) {
    if (!cpio_valid()) {
        uart_puts("ls: no initramfs loaded\n");
        return;
    }

    const char *ptr = cpio_base;
    const char *name;
    uint32_t size;

    while ((ptr = cpio_next(ptr, &name, 0, &size)) != 0) {
        /* `find .` puts the directory itself in the archive as "."; listing it
         * is noise, so skip it the way ls without -a would. */
        if (strcmp(name, ".") == 0) continue;

        uart_puts(name);
        uart_puts("\t");
        uart_dec(size);
        uart_puts("\n");
    }
}

int cpio_lookup(const char *path, const char **data, size_t *size) {
    if (!cpio_valid()) return -1;

    const char *ptr = cpio_base;
    const char *name, *file_data;
    uint32_t file_size;

    while ((ptr = cpio_next(ptr, &name, &file_data, &file_size)) != 0) {
        /* Archives built with `find .` prefix every name with "./", so accept
         * the plain name the user is likely to type as well. */
        if (strcmp(name, path) == 0 ||
            (strncmp(name, "./", 2) == 0 && strcmp(name + 2, path) == 0)) {
            if (data) *data = file_data;
            if (size) *size = file_size;
            return 0;
        }
    }

    return -1;
}

int cpio_cat(const char *path) {
    const char *data;
    size_t size;

    if (cpio_lookup(path, &data, &size) != 0) return -1;

    uart_write(data, size);
    /* Files that do not end in a newline would otherwise leave the prompt
     * glued to the last line of output. */
    if (size > 0 && data[size - 1] != '\n') uart_puts("\n");
    return 0;
}

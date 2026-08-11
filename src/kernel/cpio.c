#include "uart.h"
#include "cpio.h"

struct cpio_header{
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

static unsigned int hex8(const char *p) {
    unsigned int r = 0;
    for(volatile int i = 0; i < 8; i++){
        char c = p[i];
        unsigned int v = 0;
        if (c >= '0' && c <= '9') v = c - '0';
        else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
        r = (r << 4) | v;
    }
    return r;
}

static unsigned int align4(unsigned int x) {
    return (x + 3) & ~3u;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b) { 
        if (*a != *b) return 0; 
        a++;
        b++;
    }
    return *a == *b;
}
 

static char *cpio_next(char *ptr, char **out_name, char **out_data, unsigned int *out_size) {
    struct cpio_header *h = (struct cpio_header *)ptr;
 
    for (int i = 0; i < 6; i++)
        if (h->magic[i] != "070701"[i]) return 0;
 
    unsigned int namesize = hex8(h->namesize);
    unsigned int filesize = hex8(h->filesize);
 
    char *name = ptr + sizeof(struct cpio_header);

    char *data = ptr + align4(sizeof(struct cpio_header) + namesize);
    char *next = data + align4(filesize);

    if (str_eq(name, "TRAILER!!!")) return 0;
 
    if (out_name) *out_name = name;
    if (out_data) *out_data = data;
    if (out_size) *out_size = filesize;
    return next;
}
 
void cpio_ls(void *addr) {
    char *ptr = (char *)addr;
    char *name;
    while ((ptr = cpio_next(ptr, &name, 0, 0))) {
        uart_puts(name);
        uart_puts("\n");
    }
}
 
int cpio_cat(void *addr, const char *target) {
    char *ptr = (char *)addr;
    char *name, *data;
    unsigned int size;
    char *next;
    while ((next = cpio_next(ptr, &name, &data, &size))) {
        if (str_eq(name, target)) {
            for (unsigned int i = 0; i < size; i++) {
                if (data[i] == '\n') uart_send('\r');
                uart_send(data[i]);
            }
            return 0;
        }
        ptr = next;
    }
    return -1;
}
 
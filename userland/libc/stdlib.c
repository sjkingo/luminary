#include "stdlib.h"
#include "../syscall.h"

int atoi(const char *s)
{
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

unsigned int atou(const char *s)
{
    unsigned int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + (unsigned int)(*s++ - '0');
    return v;
}

int abs(int n)
{
    return n < 0 ? -n : n;
}

/* First-fit free-list allocator.
 * Block layout: [struct block_hdr][user data]
 * Blocks are coalesced on free. */

struct block_hdr {
    size_t          size;   /* payload size (bytes after header) */
    struct block_hdr *next;
    int             free;
};

#define HDR_SIZE  sizeof(struct block_hdr)
#define HEAP_GROW 4096      /* minimum grow amount */

static struct block_hdr *heap_head = 0;

static struct block_hdr *heap_grow(size_t need)
{
    size_t grow = (need + HDR_SIZE + HEAP_GROW - 1) & ~(HEAP_GROW - 1);
    struct block_hdr *b = (struct block_hdr *)sbrk((int)grow);
    if (b == (struct block_hdr *)-1)
        return 0;
    b->size = grow - HDR_SIZE;
    b->next = 0;
    b->free = 1;
    return b;
}

void *malloc(size_t size)
{
    if (size == 0) return 0;

    size = (size + 3) & ~3u;    /* align to 4 bytes */

    if (!heap_head) {
        heap_head = heap_grow(size);
        if (!heap_head) return 0;
    }

    struct block_hdr *b = heap_head;
    struct block_hdr *prev = 0;

    /* first-fit search */
    while (b) {
        if (b->free && b->size >= size) {
            /* split if leftover is worth it */
            if (b->size >= size + HDR_SIZE + 4) {
                struct block_hdr *split = (struct block_hdr *)((char *)b + HDR_SIZE + size);
                split->size = b->size - HDR_SIZE - size;
                split->next = b->next;
                split->free = 1;
                b->size = size;
                b->next = split;
            }
            b->free = 0;
            return (char *)b + HDR_SIZE;
        }
        prev = b;
        b = b->next;
    }

    /* no fit — grow heap */
    struct block_hdr *nb = heap_grow(size);
    if (!nb) return 0;
    prev->next = nb;

    if (nb->size >= size + HDR_SIZE + 4) {
        struct block_hdr *split = (struct block_hdr *)((char *)nb + HDR_SIZE + size);
        split->size = nb->size - HDR_SIZE - size;
        split->next = nb->next;
        split->free = 1;
        nb->size = size;
        nb->next = split;
    }
    nb->free = 0;
    return (char *)nb + HDR_SIZE;
}

void free(void *ptr)
{
    if (!ptr) return;
    struct block_hdr *b = (struct block_hdr *)((char *)ptr - HDR_SIZE);
    b->free = 1;

    /* coalesce forward */
    while (b->next && b->next->free) {
        b->size += HDR_SIZE + b->next->size;
        b->next = b->next->next;
    }
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return 0; }

    struct block_hdr *b = (struct block_hdr *)((char *)ptr - HDR_SIZE);
    size = (size + 3) & ~3u;

    if (b->size >= size)
        return ptr;

    void *np = malloc(size);
    if (!np) return 0;

    /* copy old payload */
    char *d = (char *)np;
    char *s = (char *)ptr;
    size_t copy = b->size;
    while (copy--) *d++ = *s++;

    free(ptr);
    return np;
}

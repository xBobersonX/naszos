/* Arena - patrz arena.h */

#include "arena.h"
#include "heap.h"
#include "string.h"

#define CHUNK 65536

struct arena_chunk {
    struct arena_chunk *next;
    size_t size, pos;
    size_t pad;                      /* naglowek ma 32 bajty - dane wyrownane do 16 */
    unsigned char data[];
};

void *arena_alloc(struct arena *a, size_t n)
{
    n = (n + 15) & ~(size_t)15;
    struct arena_chunk *c = a->head;
    if (!c || c->pos + n > c->size) {
        size_t size = n > CHUNK ? n : CHUNK;
        c = kmalloc(sizeof(*c) + size);
        if (!c)
            return 0;
        c->size = size;
        c->pos = 0;
        c->next = a->head;
        a->head = c;
    }
    void *p = c->data + c->pos;
    c->pos += n;
    a->used += n;
    memset(p, 0, n);
    return p;
}

char *arena_strndup(struct arena *a, const char *s, size_t n)
{
    char *d = arena_alloc(a, n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = '\0';
    }
    return d;
}

void arena_free(struct arena *a)
{
    while (a->head) {
        struct arena_chunk *next = a->head->next;
        kfree(a->head);
        a->head = next;
    }
    a->used = 0;
}

void arena_scan(struct arena *a, void (*fn)(void *ctx, const void *lo, const void *hi), void *ctx)
{
    for (struct arena_chunk *c = a->head; c; c = c->next)
        fn(ctx, c->data, c->data + c->pos);
}

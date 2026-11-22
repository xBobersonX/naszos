/* Sterta kernela (kmalloc/kfree). Sterta to ciagly obszar zaraz za mapa
 * bitowa PMM, podzielony na bloki. Kazdy blok ma 32-bajtowy naglowek i jest
 * na liscie wszystkich blokow w kolejnosci adresow - dzieki temu przy
 * zwalnianiu latwo sklejamy sasiednie wolne bloki. Gdy brakuje miejsca,
 * sterta rosnie, zajmujac kolejne ramki od PMM. */

#include "heap.h"
#include "pmm.h"
#include "string.h"
#include "kernel.h"

#define HEAP_MAGIC     0x4B4D          /* "KM" - wykrywa uszkodzone naglowki */
#define ALIGN          16
#define MIN_SPLIT      16              /* nie tworzymy blokow mniejszych niz tyle */
#define HEAP_INITIAL   (64 * 1024)

struct block {                         /* 32 bajty - dane za nim wyrownane do 16 */
    uint32_t size;                     /* rozmiar danych, bez naglowka */
    uint16_t free;
    uint16_t magic;
    struct block *next, *prev;
} __attribute__((aligned(16)));

#define HDR sizeof(struct block)
_Static_assert(sizeof(struct block) % ALIGN == 0, "naglowek bloku musi zachowac wyrownanie");

static struct block *head, *tail;
static uintptr_t heap_start, heap_end;

static inline size_t align_up(size_t n)
{
    return (n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
}

/* Powieksza sterte o co najmniej `bytes`. Zwraca 0, gdy nastepna ramka jest zajeta. */
static int heap_grow(size_t bytes)
{
    uintptr_t old_end = heap_end;
    uint32_t want = (uint32_t)((bytes + FRAME_SIZE - 1) & ~(size_t)(FRAME_SIZE - 1));

    while (heap_end - old_end < want && pmm_claim(heap_end))
        heap_end += FRAME_SIZE;
    uint32_t added = (uint32_t)(heap_end - old_end);
    if (added == 0)
        return 0;

    if (tail && tail->free) {
        tail->size += added;
    } else {
        struct block *b = (struct block *)old_end;
        b->size  = added - HDR;
        b->free  = 1;
        b->magic = HEAP_MAGIC;
        b->next  = 0;
        b->prev  = tail;
        if (tail)
            tail->next = b;
        else
            head = b;
        tail = b;
    }
    return added >= want;
}

void heap_init(void)
{
    heap_start = heap_end = pmm_first_free_addr();
    head = tail = 0;
    if (!heap_grow(HEAP_INITIAL))
        panic("heap_init: nie mozna zarezerwowac pamieci na sterte");
}

static void split(struct block *b, size_t size)
{
    if (b->size < size + HDR + MIN_SPLIT)
        return;
    struct block *n = (struct block *)((uint8_t *)(b + 1) + size);
    n->size  = b->size - size - HDR;
    n->free  = 1;
    n->magic = HEAP_MAGIC;
    n->prev  = b;
    n->next  = b->next;
    if (b->next)
        b->next->prev = n;
    else
        tail = n;
    b->next = n;
    b->size = size;
}

void *kmalloc(size_t size)
{
    if (size == 0 || size > 0x7FFFFFFF)   /* duze wartosci przepelnilyby align_up */
        return 0;
    size = align_up(size);

    for (int attempt = 0; attempt < 2; attempt++) {
        for (struct block *b = head; b; b = b->next) {      /* first-fit */
            if (b->free && b->size >= size) {
                split(b, size);
                b->free = 0;
                return b + 1;
            }
        }
        size_t need = size + HDR;
        if (tail && tail->free)
            need -= tail->size + HDR;
        if (!heap_grow(need))
            break;
    }
    return 0;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

/* Dolacza b->next do b (oba musza byc wolne i lezec obok siebie) */
static void merge_with_next(struct block *b)
{
    struct block *n = b->next;
    b->size += HDR + n->size;
    b->next = n->next;
    if (n->next)
        n->next->prev = b;
    else
        tail = b;
    n->magic = 0;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;
    struct block *b = (struct block *)ptr - 1;
    if ((uintptr_t)b < heap_start || (uintptr_t)ptr >= heap_end || b->magic != HEAP_MAGIC)
        panic("kfree: wskaznik nie pochodzi z kmalloc albo sterta jest uszkodzona");
    if (b->free)
        panic("kfree: podwojne zwolnienie tego samego bloku");

    b->free = 1;
    if (b->next && b->next->free)
        merge_with_next(b);
    if (b->prev && b->prev->free)
        merge_with_next(b->prev);
}

void heap_get_stats(struct heap_stats *s)
{
    memset(s, 0, sizeof(*s));
    s->total = (uint32_t)(heap_end - heap_start);
    for (struct block *b = head; b; b = b->next) {
        s->blocks++;
        if (b->free) {
            s->free += b->size;
            s->free_blocks++;
            if (b->size > s->largest_free)
                s->largest_free = b->size;
        } else {
            s->used += b->size;
        }
    }
}

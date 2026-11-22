#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>
#include <stdint.h>

struct heap_stats {
    uint32_t total;         /* bajty zajete przez sterte (z naglowkami) */
    uint32_t used;          /* bajty przydzielone programom */
    uint32_t free;          /* bajty w wolnych blokach */
    uint32_t blocks;
    uint32_t free_blocks;
    uint32_t largest_free;
};

void  heap_init(void);
void *kmalloc(size_t size);          /* wyrownane do 16 bajtow; NULL gdy brak pamieci */
void *kzalloc(size_t size);          /* jak kmalloc, ale wyzerowane */
void  kfree(void *ptr);
void  heap_get_stats(struct heap_stats *s);

#endif

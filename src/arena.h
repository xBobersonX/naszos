#ifndef ARENA_H
#define ARENA_H

#include <stddef.h>

/* "Arena" - pamiec przydzielana hurtem. Zamiast tysiecy malych kmalloc
 * (kazdy przeszukuje sterte) bierzemy duze kawalki po 64 KB i kroimy je
 * po kolei, jak chleb. Zwalniamy wszystko naraz - np. gdy przegladarka
 * otwiera nowa strone. Idealne dla drzewa DOM i regul CSS. */

struct arena_chunk;

struct arena {
    struct arena_chunk *head;
    size_t used;                     /* ile bajtow razem (do statystyk) */
};

void *arena_alloc(struct arena *a, size_t n);        /* wyzerowane, wyrownane do 16 */
char *arena_strndup(struct arena *a, const char *s, size_t n);
void  arena_free(struct arena *a);

#endif

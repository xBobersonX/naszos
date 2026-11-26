#ifndef IMAGE_H
#define IMAGE_H

#include <stdint.h>

/* Obrazki z internetu: GIF, PNG, JPEG (src/image.c) i WebP (src/webp.c). Wynik to tablica
 * pikseli 0xRRGGBB; przezroczystosc zamieniamy na biale tlo (jak strona). */

struct image {
    int w, h;
    uint32_t *px;                /* kmalloc, w * h pikseli, wiersz po wierszu */
    int alpha;                   /* 1 = piksele 0xAARRGGBB z przezroczystoscia (SVG) */
};

#define IMG_EFORMAT      -1      /* nieznany format (np. SVG, AVIF) */
#define IMG_ECORRUPT     -2      /* uszkodzony plik */
#define IMG_ENOMEM       -3
#define IMG_EPROGRESSIVE -4      /* (juz nieuzywane - JPEG progresywny dziala) */
#define IMG_ETOOBIG      -5

/* Dekoduje i zmniejsza (z zachowaniem proporcji), zeby zmiescil sie
 * w max_w x max_h. 0 = OK */
int  image_decode(const uint8_t *data, unsigned len, struct image *out, int max_w, int max_h);
void image_free(struct image *img);
/* Surowy DEFLATE (PNG, gzip w HTTP): wynik = liczba bajtow wyjscia albo -1 */
int  inflate_raw(const uint8_t *in, unsigned in_len, uint8_t *out, unsigned out_len);
const char *image_strerror(int err);

/* "data:image/png;base64,iVBOR..." -> bajty (kmalloc). Zwraca dlugosc albo 0 */
unsigned base64_decode(const char *in, uint8_t **out);

#endif

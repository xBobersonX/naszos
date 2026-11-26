#ifndef WEBP_H
#define WEBP_H

#include <stdint.h>
#include "image.h"

/* Obrazki WebP (src/webp.c): "VP8 " (stratny, jak JPEG), "VP8L" (bezstratny,
 * jak PNG), "VP8X" z kanalem "ALPH" (przezroczystosc). Z animacji bierzemy
 * pierwsza klatke. Wynik jak w image.h; przy przezroczystosci img->alpha = 1
 * i piksele 0xAARRGGBB. Zwraca 0 albo IMG_E... */
int webp_decode(const uint8_t *data, unsigned len, struct image *img);

#endif

#ifndef FONT_H
#define FONT_H

#include <stdint.h>

/* Ladne czcionki (DejaVu Sans) z polskimi literami i wygladzanymi
 * krawedziami - dla przegladarki i okien. Terminal dalej uzywa czcionki
 * VGA 8x16. Tekst podajemy w UTF-8 (tak jak na stronach WWW).
 * Kazdy znak ma wlasna szerokosc ("i" jest wezsze niz "m"). */

enum { FONT_SMALL, FONT_REGULAR, FONT_BOLD, FONT_ITALIC, FONT_H2, FONT_H1, FONT_COUNT };

struct font_glyph {
    uint16_t cp;                /* kod znaku (Unicode) */
    int8_t xoff;                /* przesuniecie bitmapy wzgledem pozycji pisania */
    uint8_t yoff, w, h;         /* yoff - od gory linii */
    uint8_t adv;                /* o ile przesunac sie do nastepnego znaku */
    uint32_t off;               /* poczatek bitmapy w tablicy alpha */
};

struct font_face {
    uint8_t height, ascent;     /* wysokosc linii i czesc nad linia bazowa */
    uint16_t count;
    const struct font_glyph *glyphs;   /* posortowane po kodzie znaku */
    const uint8_t *alpha;       /* przezroczystosc pikseli (0-255) */
};

extern const struct font_face font_faces[FONT_COUNT];   /* src/font_data.c */

int font_height(int font);
int font_ascent(int font);
/* Szerokosc tekstu w pikselach (len bajtow UTF-8) */
int font_width(int font, const char *utf8, unsigned len);
/* Rysuje tekst: (x, y) = lewy gorny rog linii. Zwraca szerokosc. */
int font_draw(int font, int x, int y, const char *utf8, unsigned len, uint32_t color);
/* Bitmapa jednego znaku (z zastepstwem: litera bez ogonka albo "?") - dla <canvas> */
const struct font_glyph *font_glyph(int font, uint32_t cp);
/* Ile bajtow tekstu miesci sie w max_w pikselach (do obcinania) */
unsigned font_fit(int font, const char *utf8, unsigned len, int max_w);

#endif

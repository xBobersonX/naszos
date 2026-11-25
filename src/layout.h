#ifndef LAYOUT_H
#define LAYOUT_H

#include <stdint.h>
#include "dom.h"
#include "css.h"
#include "image.h"

/* Uklad strony (src/layout.c): drzewo DOM z obliczonym wygladem CSS ->
 * lista rzeczy do narysowania ("display list") w pikselach od gory strony:
 * prostokaty (tla, ramki), teksty, obrazki. Przegladarka tylko ja rysuje
 * i sprawdza, w co kliknieto. */

enum { DL_RECT, DL_TEXT, DL_IMAGE, DL_HIDDEN, DL_CANVAS /* node->canvas */, DL_GRAD /* tlo: node->style->grad */,
       DL_RING /* ramka z zaokraglonymi rogami */, DL_SHADOW /* box-shadow */ };
#define FONT_MONO 255                /* tekst <pre> - czcionka VGA 8x16 */

struct dl_item {
    uint8_t kind, font, underline;
    uint8_t fixed;                   /* (w trakcie ukladu) float - linia go nie przesuwa */
    int16_t img;                     /* DL_IMAGE: numer obrazka */
    int16_t asc;                     /* (w trakcie ukladu) ile nad linia bazowa */
    int16_t ul_left;                 /* podkreslenie siega tyle px w lewo (spacja w linku) */
    int32_t link;                    /* numer linku albo -1 */
    int x, y, w, h;
    int16_t rad[4];                  /* zaokraglenie rogow (DL_RECT, DL_IMAGE, DL_RING, DL_SHADOW); 0 = ostre */
    int16_t ext[4];                  /* DL_RING: grubosci ramki; DL_SHADOW: rozmycie, rozszerzenie, przesuniecie x, y */
    uint8_t alpha;                   /* DL_SHADOW: przezroczystosc koloru */
    int rel;                         /* (w trakcie ukladu) polozenie wzgledem gory "atomu" */
    uint32_t color;
    const char *text;
    uint16_t len;
    struct dom_node *node;           /* element, z ktorego pochodzi (klikniecia dla JavaScriptu) */
};

struct page_layout {
    struct dl_item *items;
    unsigned n, cap;
    const char **links;              /* adresy linkow (href) */
    unsigned nlinks, links_cap;
    int width, height;
    uint32_t bg;                     /* kolor tla strony */
    struct arena tmp;                /* pamiec pomocnicza jednego ukladu */
    unsigned gen;
};

void layout_page(struct page_layout *L, struct dom_doc *d, int width,
                 const struct image *imgs, int nimgs);
void layout_free(struct page_layout *L);

#endif

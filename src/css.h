#ifndef CSS_H
#define CSS_H

#include <stdint.h>
#include "arena.h"
#include "dom.h"

/* CSS - wyglad strony (src/css.c). Arkusz stylow to lista regul:
 *     selektor { wlasnosc: wartosc; ... }      np.  .menu a { color: red }
 * Dla kazdego elementu DOM szukamy pasujacych regul, ukladamy je wedlug
 * "sily" (kaskada: #id > .klasa > znacznik, pozniejsza wygrywa) i liczymy
 * gotowy wyglad (struct style). Kolor, czcionka itp. dziedzicza dzieci. */

/* Dlugosc: px, procent (v = procent * 100) albo auto */
enum { L_AUTO, L_PX, L_PCT };
struct css_len {
    int32_t v;
    uint8_t u;
};

enum {
    D_NONE, D_INLINE, D_BLOCK, D_LIST_ITEM, D_INLINE_BLOCK,
    D_TABLE, D_ROW_GROUP, D_ROW, D_CELL, D_CAPTION, D_FLEX,
};
enum { JU_START, JU_CENTER, JU_END, JU_BETWEEN, JU_AROUND };
enum { FL_NONE, FL_LEFT, FL_RIGHT };
enum { CL_NONE, CL_LEFT, CL_RIGHT, CL_BOTH };
enum { AL_LEFT, AL_CENTER, AL_RIGHT };
enum { LS_NONE, LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL };
enum { WS_NORMAL, WS_NOWRAP, WS_PRE, WS_PRE_LINE };
enum { POS_STATIC, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED };

#define GRID_COLS 16
enum { GT_PX, GT_PCT, GT_FR, GT_AUTO };

struct style {
    uint8_t display, floating, clear, align, list, white_space;
    uint8_t bold, italic, underline, hidden, overflow, out_of_flow;
    uint8_t font;                        /* FONT_* - dobrana czcionka */
    uint8_t border_box;                  /* box-sizing: border-box - szerokosc razem z ramka */
    uint8_t has_bg;
    int16_t font_px;                     /* rozmiar czcionki w pikselach */
    int16_t line_px;                     /* wysokosc linii (0 = zwykla) */
    uint32_t color, bg;
    struct css_len width, height, max_width, min_width;
    struct css_len margin[4], padding[4];            /* gora, prawo, dol, lewo */
    int16_t border[4];
    int16_t border_w[4];                 /* border-width podane bez stylu (czeka na border-style) */
    uint8_t border_w_set;                /* bity 0..3: ktore border_w sa podane */
    uint32_t border_color[4];
    int16_t border_spacing;
    /* flexbox: elementy w rzedzie (menu, zakladki, kolumny) */
    uint8_t flex_column, flex_wrap, justify, flex_grow, flex_shrink_none;
    int16_t gap;
    struct css_len flex_basis;
    /* position: relative/absolute/fixed i przesuniecia top/right/bottom/left */
    uint8_t position;
    struct css_len inset[4];             /* gora, prawo, dol, lewo (auto = nie podane) */
    struct css_len tx, ty;               /* transform: translate(x, y) */
    /* border-radius: lewy-gorny, prawy-gorny, prawy-dolny, lewy-dolny (px albo %) */
    struct css_len radius[4];
    /* box-shadow (pierwszy cien z listy); alpha 0 = brak cienia */
    struct { int16_t x, y, blur, spread; uint32_t color; uint8_t alpha; } shadow;
    /* SVG: fill/stroke z CSS na tym elemencie (bit 0 fill, 1 stroke, 2 stroke-width) */
    uint8_t svg_set, fill_kind, stroke_kind;     /* kind: 0 kolor, 1 none, 2 currentColor */
    uint8_t fill_a, stroke_a;
    uint32_t fill, stroke;
    uint16_t stroke_w16;                         /* grubosc obrysu w 1/16 px */
    /* CSS Grid: display grid (blok z flaga) i kolumny grid-template-columns */
    uint8_t grid;                                /* 1 = display: grid / inline-grid */
    uint8_t grid_n;                              /* ile kolumn (0 = jedna) */
    uint8_t grid_fill;                           /* repeat(auto-fill / auto-fit, ...) */
    uint8_t grid_u[GRID_COLS];                   /* GT_* */
    int16_t grid_v[GRID_COLS];                   /* px, procent * 100 albo fr * 100 */
    int16_t grid_min[GRID_COLS];                 /* minmax(min, ...) w px */
    int16_t row_gap;
    int8_t col_start, col_end;                   /* grid-column: 1 / -1 (0 = auto) */
    uint8_t col_span;                            /* grid-column: span 2 */
};

struct css_ctx;

/* viewport_w: szerokosc okna - dla regul @media (min-width: ...) */
struct css_ctx *css_new(int viewport_w);
/* Dodaje arkusz (tresc pliku .css albo znacznika <style>) */
void css_add(struct css_ctx *c, const char *text, unsigned len);
/* Liczy wyglad wszystkich elementow dokumentu (n->style) */
void css_compute(struct css_ctx *c, struct dom_doc *d);
void css_free(struct css_ctx *c);
unsigned css_rule_count(const struct css_ctx *c);

/* Selektor dla JavaScriptu (querySelector, matches). NULL = nieobslugiwany.
 * Pamiec z areny kontekstu CSS (zwalniana z nim). */
struct css_sel;
struct css_sel *css_sel_parse(struct css_ctx *c, const char *text);
int css_sel_match(const struct css_sel *sel, const struct dom_node *n);
/* Kolor po nazwie CSS (np. "orange") -> 0xRRGGBB. 0 = nieznana nazwa */
int css_named_color(const char *name, uint32_t *rgb);

#endif

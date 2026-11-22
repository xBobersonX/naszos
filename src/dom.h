#ifndef DOM_H
#define DOM_H

#include <stdint.h>
#include "arena.h"

/* DOM - strona jako drzewo (src/dom.c). Kazdy znacznik to wezel z dziecmi:
 *   <html> -> <body> -> <div class="x"> -> <p> -> "tekst"
 * Z drzewa korzystaja style CSS (css.c) i uklad strony (layout.c). */

enum { N_ELEMENT, N_TEXT };

/* Znaczniki, ktore traktujemy w specjalny sposob */
enum {
    T_OTHER, T_HTML, T_HEAD, T_BODY, T_TITLE, T_STYLE, T_SCRIPT, T_LINK, T_META,
    T_A, T_IMG, T_BR, T_HR, T_P, T_DIV, T_SPAN, T_UL, T_OL, T_LI, T_DL, T_DT, T_DD,
    T_TABLE, T_THEAD, T_TBODY, T_TFOOT, T_TR, T_TD, T_TH, T_CAPTION,
    T_H1, T_H2, T_H3, T_H4, T_H5, T_H6, T_PRE, T_BLOCKQUOTE, T_B, T_STRONG, T_I, T_EM,
    T_INPUT, T_BUTTON, T_SELECT, T_OPTION, T_TEXTAREA, T_FORM, T_NOSCRIPT, T_TEMPLATE,
    T_SVG, T_IFRAME, T_CENTER, T_FONT, T_SMALL, T_SUP, T_SUB, T_LABEL, T_NAV, T_CANVAS, T_COUNT,
};

/* Piksele elementu <canvas> (rysuje po nich JavaScript - js_canvas.c,
 * przegladarka tylko je pokazuje). Pamiec z kmalloc, zwalniana z dokumentem. */
struct dom_canvas {
    int w, h;
    uint32_t *px;                /* w * h pikseli 0xAARRGGBB (alfa 0 = przezroczysty) */
    struct dom_canvas *next;     /* lista wszystkich plocien dokumentu */
};
#define CANVAS_MAX_PIXELS (4096 * 2048)

struct dom_attr {
    const char *name, *value;
    struct dom_attr *next;
};

struct style;

struct dom_node {
    uint8_t type;
    uint8_t tag;                 /* T_* */
    const char *name;            /* nazwa znacznika malymi literami */
    const char *id, *cls;        /* atrybuty id i class (albo NULL) */
    struct dom_attr *attrs;
    const char *text;            /* wezel tekstowy: UTF-8 */
    unsigned text_len;
    struct dom_node *parent, *first, *last, *next, *prev;
    struct style *style;         /* obliczony wyglad (css.c) */
    int img;                     /* <img>: numer w dom_doc.images, inaczej -1 */
    int mmin, mmax;              /* zapamietana najmniejsza i najwieksza szerokosc (layout.c) */
    unsigned mgen;
    void *js;                    /* obiekt JavaScriptu tego wezla (js_dom.c) albo NULL */
    uint8_t script_done;         /* <script> juz wykonany */
    struct dom_canvas *canvas;   /* <canvas>: piksele (dopiero gdy skrypt zacznie rysowac) */
};

struct dom_doc {
    struct arena mem;            /* cala pamiec dokumentu - zwalniana naraz */
    struct dom_node *root;
    char title[96];
    const char **images;         /* adresy obrazkow (src) */
    struct dom_node **image_nodes;   /* <img> kazdego adresu (zdarzenie load dla JavaScriptu) */
    int nimages, images_cap;
    struct dom_canvas *canvases; /* plotna <canvas> (do zwolnienia) */
    volatile unsigned canvas_gen;    /* rosnie po kazdym rysowaniu - trzeba odswiezyc okno */
    const char **sheets;         /* adresy arkuszy stylow (<link rel=stylesheet>) */
    int nsheets;
    char *css;                   /* tresc znacznikow <style> (sklejona) */
    unsigned css_len, css_cap;
    unsigned nodes;
    struct dom_node **scripts;   /* znaczniki <script> w kolejnosci (JavaScript) */
    int nscripts, scripts_cap;
};

struct dom_doc *dom_parse(const char *src, unsigned len, int is_html, int charset);
/* Jak dom_parse, ale przy kazdym </script> wola on_script (JavaScript wykonuje
 * skrypt, zanim parser pojdzie dalej - jak w prawdziwej przegladarce) */
typedef void (*dom_script_fn)(void *ctx, struct dom_doc *d, struct dom_node *script);
struct dom_doc *dom_parse_scripts(const char *src, unsigned len, int is_html, int charset,
                                  dom_script_fn on_script, void *ctx);
void dom_free(struct dom_doc *d);

const char *dom_attr(const struct dom_node *n, const char *name);   /* NULL = brak */
int dom_has_class(const struct dom_node *n, const char *cls, unsigned len);

/* Zmiany drzewa (JavaScript). Pamiec z areny dokumentu. */
/* HTML -> nowe wezly: dzieci zwroconego wezla "#fragment" (skrypty sie nie wykonuja) */
struct dom_node *dom_parse_fragment(struct dom_doc *d, const char *html, unsigned len);
struct dom_node *dom_new_element(struct dom_doc *d, const char *name);
struct dom_node *dom_new_text(struct dom_doc *d, const char *text, unsigned len);
void dom_append(struct dom_node *parent, struct dom_node *n);
void dom_insert_before(struct dom_node *parent, struct dom_node *n, struct dom_node *ref);
void dom_remove(struct dom_node *n);                        /* odlacza od rodzica */
void dom_set_attr(struct dom_doc *d, struct dom_node *n, const char *name, const char *value);
void dom_remove_attr(struct dom_node *n, const char *name);
int  dom_tag_id(const char *name);
const char *dom_tag_name(int tag);
/* <img> dostal nowy adres - dopisuje go do listy obrazkow dokumentu */
void dom_add_image(struct dom_doc *d, struct dom_node *n);
/* Rozmiar plotna z atrybutow width/height (domyslnie 300 x 150) */
void dom_canvas_dims(const struct dom_node *n, int *w, int *h);
/* Piksele plotna o rozmiarze z atrybutow - nowe (wyczyszczone), gdy rozmiar sie
 * zmienil. NULL = brak pamieci albo za duze plotno */
struct dom_canvas *dom_canvas_sync(struct dom_doc *d, struct dom_node *n);

#endif

#ifndef HTML_H
#define HTML_H

#include <stdint.h>

/* Zamiana strony HTML na liste "klockow": slowa (z wygladem i linkiem),
 * konce linii, akapity, punkty listy, poziome kreski. Przegladarka uklada
 * z nich tekst w oknie, a komenda "www" - zwykly tekst w terminalu.
 * Tekst trzymamy w UTF-8 (z polskimi literami) - dla terminala html_to_plain
 * zamienia go na litery bez ogonkow. Skrypty i style pomijamy. */

enum {
    HT_WORD,        /* slowo (tekst bez spacji, albo linia <pre>) */
    HT_BREAK,       /* nowa linia (<br>) */
    HT_PARA,        /* nowy akapit - odstep */
    HT_BULLET,      /* punkt listy (<li>) */
    HT_RULE,        /* pozioma kreska (<hr>) */
    HT_IMAGE,       /* obrazek: numer w doc->images, tekst = opis (alt) */
};

enum {
    HS_NORMAL,
    HS_H1,          /* duzy naglowek */
    HS_H2,          /* mniejszy naglowek */
    HS_BOLD,
    HS_PRE,         /* tekst o stalej szerokosci, bez zawijania */
    HS_IMAGE,       /* [obrazek] - opis zamiast obrazka */
    HS_ITALIC,      /* kursywa (<i>, <em>) */
};

struct html_token {
    uint8_t type;
    uint8_t style;
    uint8_t space;          /* przed slowem byla spacja */
    int16_t link;           /* numer linku albo -1 */
    int16_t img;            /* numer obrazka (HT_IMAGE) */
    uint32_t text;          /* poczatek tekstu w doc->text */
    uint16_t len;
};

struct html_doc {
    struct html_token *tok;
    unsigned ntok, tok_cap;
    char *text;
    unsigned text_len, text_cap;
    char **links;           /* adresy linkow (href), jak na stronie */
    unsigned nlinks, links_cap;
    char **images;          /* adresy obrazkow (src), jak na stronie */
    unsigned nimages, images_cap;
    char title[96];
};

/* Kodowanie znakow strony - jak zapisane sa litery spoza ASCII (np. polskie) */
enum { HC_UTF8, HC_LATIN2, HC_CP1250, HC_LATIN1 };
/* Z naglowka Content-Type albo <meta charset=...> na poczatku strony */
int html_charset(const char *content_type, const char *src, unsigned len);

struct html_doc *html_parse(const char *src, unsigned len, int is_html, int charset);   /* NULL = brak pamieci */
void html_free(struct html_doc *d);

/* Narzedzia wspolne z parserem DOM (src/dom.c): */
/* "&amp;", "&#x105;" -> kod znaku (0 = to nie encja); *s wskazuje na '&' */
uint32_t html_entity(const char **s, const char *end);
/* Wartosc atrybutu (np. "href") z tekstu atrybutow znacznika. 1 = jest */
int html_attr(const char *attrs, const char *end, const char *name, char *out, unsigned size);
/* Bajt w starym kodowaniu (ISO-8859-2 itp.) -> kod znaku */
uint32_t html_legacy_char(uint8_t c, int charset);

/* Caly dokument jako zwykly tekst (kmalloc) - dla terminala */
char *html_to_plain(const struct html_doc *d, unsigned *out_len);

#endif

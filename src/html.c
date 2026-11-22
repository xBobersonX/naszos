/* Rozbior HTML - patrz html.h.
 *
 * Idziemy po stronie znak po znaku. Tekst dzielimy na slowa, a znaczniki
 * <...> zmieniaja "stan": <b> wlacza pogrubienie, <a href=...> zaczyna link,
 * <p> konczy akapit itd. Nie budujemy calego drzewa strony (DOM) jak prawdziwe
 * przegladarki - do czytania tekstu i klikania linkow to wystarczy. */

#include "html.h"
#include "heap.h"
#include "string.h"
#include "http.h"
#include "thread.h"
#include "utf8.h"

#define WORD_MAX 255

struct parser {
    struct html_doc *d;
    int ok;                      /* 0 = zabraklo pamieci */
    int bold, italic, heading, pre, in_title;
    int charset;
    int link;                    /* biezacy link albo -1 */
    int space;                   /* przed nastepnym slowem jest odstep */
    char word[WORD_MAX + 1];
    unsigned wlen;
    unsigned title_len;
};

/* ---------- budowanie dokumentu ---------- */

static int grow(void **buf, unsigned *cap, unsigned need, unsigned item)
{
    if (need <= *cap)
        return 1;
    unsigned n = *cap ? *cap * 2 : 256;
    while (n < need)
        n *= 2;
    void *p = kmalloc((size_t)n * item);
    if (!p)
        return 0;
    if (*buf) {
        memcpy(p, *buf, (size_t)*cap * item);
        kfree(*buf);
    }
    *buf = p;
    *cap = n;
    return 1;
}

static struct html_token *new_token(struct parser *p, uint8_t type)
{
    struct html_doc *d = p->d;
    if (!p->ok || !grow((void **)&d->tok, &d->tok_cap, d->ntok + 1, sizeof(struct html_token))) {
        p->ok = 0;
        return 0;
    }
    struct html_token *t = &d->tok[d->ntok++];
    memset(t, 0, sizeof(*t));
    t->type = type;
    t->link = -1;
    return t;
}

static uint8_t current_style(const struct parser *p)
{
    if (p->pre) return HS_PRE;
    if (p->heading == 1) return HS_H1;
    if (p->heading == 2) return HS_H2;
    if (p->bold || p->heading == 3) return HS_BOLD;
    if (p->italic) return HS_ITALIC;
    return HS_NORMAL;
}

static void add_word(struct parser *p, const char *s, unsigned len, uint8_t style)
{
    struct html_doc *d = p->d;
    if (!len)
        return;
    if (!p->ok || !grow((void **)&d->text, &d->text_cap, d->text_len + len + 1, 1)) {
        p->ok = 0;
        return;
    }
    struct html_token *t = new_token(p, HT_WORD);
    if (!t)
        return;
    t->style = style;
    t->space = (uint8_t)p->space;
    t->link = (int16_t)p->link;
    t->text = d->text_len;
    t->len = (uint16_t)len;
    memcpy(d->text + d->text_len, s, len);
    d->text_len += len;
    d->text[d->text_len] = '\0';
    p->space = 0;
}

static void flush_word(struct parser *p)
{
    if (p->wlen) {
        add_word(p, p->word, p->wlen, current_style(p));
        p->wlen = 0;
    }
}

/* Nowa linia / akapit / punkt / kreska - bez pustych linii na poczatku
 * i bez wielu akapitow pod rzad */
static void add_break(struct parser *p, uint8_t type)
{
    flush_word(p);
    struct html_doc *d = p->d;
    if (d->ntok == 0 && type != HT_RULE && type != HT_BULLET)
        return;
    if (d->ntok) {
        struct html_token *last = &d->tok[d->ntok - 1];
        if ((type == HT_PARA || type == HT_BREAK) && (last->type == HT_PARA || last->type == HT_BULLET))
            return;                                  /* np. <li><div>: bez pustej linii po punkcie */
        if (type == HT_PARA && last->type == HT_BREAK) {
            last->type = HT_PARA;
            return;
        }
        if (type == HT_BREAK && last->type == HT_PARA)
            return;
        if (type == HT_BREAK && last->type == HT_BREAK && d->ntok >= 2 &&
            d->tok[d->ntok - 2].type == HT_BREAK)
            return;                                  /* najwyzej jedna pusta linia */
    }
    new_token(p, type);
    p->space = 0;
}

static void put_char(struct parser *p, char c)
{
    if (p->in_title) {
        if (c == ' ' && (p->title_len == 0 || p->d->title[p->title_len - 1] == ' '))
            return;
        if (p->title_len + 1 < sizeof(p->d->title)) {
            p->d->title[p->title_len++] = c;
            p->d->title[p->title_len] = '\0';
        }
        return;
    }
    if (p->pre) {
        if (c == '\n') {
            flush_word(p);
            new_token(p, HT_BREAK);
            return;
        }
        if (c == '\t') {
            for (int i = 0; i < 4; i++)
                put_char(p, ' ');
            return;
        }
    } else if (c == ' ') {
        flush_word(p);
        p->space = 1;
        return;
    }
    if (p->wlen >= WORD_MAX)
        flush_word(p);
    p->word[p->wlen++] = c;
}

/* ---------- znaki ---------- */

/* Znak Unicode trafia do slowa jako UTF-8 (ladna czcionka ma polskie litery).
 * Odstepy "specjalne" (np. twarda spacja) sa zwyklymi spacjami. */
static void put_codepoint(struct parser *p, uint32_t cp)
{
    if (cp == '\t' || cp == '\r' || cp == '\n' || cp == '\f') {
        put_char(p, p->pre && cp != '\r' ? (char)cp : ' ');
        return;
    }
    if (cp == 0xA0 || cp == 0x2009 || cp == 0x2002 || cp == 0x2003) {
        put_char(p, ' ');
        return;
    }
    if (cp == 0x200B || cp == 0xFEFF || cp == 0xAD || cp < 32)   /* niewidoczne */
        return;
    char buf[4];
    unsigned n = utf8_put(cp, buf);
    if (p->wlen + n > WORD_MAX)
        flush_word(p);
    for (unsigned i = 0; i < n; i++)
        put_char(p, buf[i]);
}

/* Polskie litery w starych kodowaniach jednobajtowych: bajt -> kod Unicode */
uint32_t html_legacy_char(uint8_t c, int charset)
{
    static const struct { uint8_t byte; uint16_t cp; } latin2[] = {
        { 0xA1, 0x104 }, { 0xB1, 0x105 }, { 0xC6, 0x106 }, { 0xE6, 0x107 }, { 0xCA, 0x118 },
        { 0xEA, 0x119 }, { 0xA3, 0x141 }, { 0xB3, 0x142 }, { 0xD1, 0x143 }, { 0xF1, 0x144 },
        { 0xD3, 0x0D3 }, { 0xF3, 0x0F3 }, { 0xA6, 0x15A }, { 0xB6, 0x15B }, { 0xAC, 0x179 },
        { 0xBC, 0x17A }, { 0xAF, 0x17B }, { 0xBF, 0x17C }, { 0xA0, 0x0A0 }, { 0xE9, 0x0E9 },
        { 0xF6, 0x0F6 }, { 0xFC, 0x0FC }, { 0xE4, 0x0E4 }, { 0xD6, 0x0D6 }, { 0xDC, 0x0DC },
    };
    if (c < 0x80 || charset == HC_LATIN1)
        return c;
    if (charset == HC_CP1250) {          /* rozni sie od ISO-8859-2 kilkoma literami */
        switch (c) {
        case 0xA5: return 0x104;
        case 0xB9: return 0x105;
        case 0x8C: return 0x15A;
        case 0x9C: return 0x15B;
        case 0x8F: return 0x179;
        case 0x9F: return 0x17A;
        case 0x96: return 0x2013;
        case 0x97: return 0x2014;
        case 0x84: return 0x201E;
        case 0x94: return 0x201D;
        case 0xA1: case 0xB1: case 0xA6: case 0xB6: case 0xAC: case 0xBC: return 0xFFFD;
        }
    }
    for (unsigned i = 0; i < sizeof(latin2) / sizeof(latin2[0]); i++)
        if (latin2[i].byte == c)
            return latin2[i].cp;
    return 0xFFFD;
}

int html_charset(const char *content_type, const char *src, unsigned len)
{
    char buf[120];
    const char *cs = 0;
    if (content_type) {
        for (const char *s = content_type; *s; s++)
            if (str_starts_ci(s, "charset=")) {
                cs = s + 8;
                break;
            }
    }
    if (!cs) {                           /* <meta charset="..."> w pierwszych 2 KB */
        unsigned n = len < 2048 ? len : 2048;
        for (unsigned i = 0; i + 8 < n; i++)
            if (str_starts_ci(src + i, "charset=")) {
                unsigned k = 0;
                const char *v = src + i + 8;
                while (k + 1 < sizeof(buf) && v < src + n && *v != '>')
                    buf[k++] = *v++;
                buf[k] = '\0';
                cs = buf;
                while (*cs == '"' || *cs == '\'')
                    cs++;
                break;
            }
    }
    if (!cs)
        return HC_UTF8;
    if (str_starts_ci(cs, "iso-8859-2") || str_starts_ci(cs, "latin2"))
        return HC_LATIN2;
    if (str_starts_ci(cs, "windows-1250") || str_starts_ci(cs, "cp1250"))
        return HC_CP1250;
    if (str_starts_ci(cs, "iso-8859-1") || str_starts_ci(cs, "windows-1252") || str_starts_ci(cs, "us-ascii"))
        return HC_LATIN1;
    return HC_UTF8;
}

/* &amp; &lt; &#39; &#x105; -> kod znaku. *s wskazuje na '&'. 0 = to nie encja */
uint32_t html_entity(const char **s, const char *end)
{
    static const struct { const char *name; uint32_t cp; } names[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
        { "nbsp", 0xA0 }, { "copy", 0xA9 }, { "reg", 0xAE }, { "hellip", 0x2026 },
        { "ndash", 0x2013 }, { "mdash", 0x2014 }, { "oacute", 0xF3 }, { "Oacute", 0xD3 },
        { "raquo", 0xBB }, { "laquo", 0xAB }, { "bull", 0x2022 }, { "middot", 0xB7 },
        { "rsquo", 0x2019 }, { "lsquo", 0x2018 }, { "rdquo", 0x201D }, { "ldquo", 0x201C },
        { "times", 0xD7 }, { "rarr", 0x2192 }, { "larr", 0x2190 }, { "euro", 0x20AC },
        { "deg", 0xB0 }, { "eacute", 0xE9 }, { "uuml", 0xFC }, { "ouml", 0xF6 }, { "auml", 0xE4 },
    };
    const char *q = *s + 1;
    uint32_t cp = 0;
    if (q < end && *q == '#') {
        q++;
        int hex = q < end && (*q == 'x' || *q == 'X');
        if (hex)
            q++;
        int digits = 0;
        for (; q < end; q++, digits++) {
            char c = *q;
            if (c >= '0' && c <= '9') cp = cp * (hex ? 16 : 10) + (uint32_t)(c - '0');
            else if (hex && c >= 'a' && c <= 'f') cp = cp * 16 + (uint32_t)(c - 'a' + 10);
            else if (hex && c >= 'A' && c <= 'F') cp = cp * 16 + (uint32_t)(c - 'A' + 10);
            else break;
            if (digits > 7)
                return 0;
        }
        if (!digits)
            return 0;
        if (q < end && *q == ';')
            q++;
        *s = q;
        return cp ? cp : 0xFFFD;
    }
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        unsigned l = (unsigned)strlen(names[i].name);
        if (q + l < end && memcmp(q, names[i].name, l) == 0 && q[l] == ';') {
            *s = q + l + 1;
            return names[i].cp;
        }
    }
    return 0;
}

/* ---------- znaczniki ---------- */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

/* Wartosc atrybutu (np. href) z tekstu atrybutow znacznika, z encjami zamienionymi na znaki */
int html_attr(const char *a, const char *end, const char *name, char *out, unsigned size)
{
    unsigned nl = (unsigned)strlen(name);
    while (a < end) {
        while (a < end && (*a == ' ' || *a == '\t' || *a == '\n' || *a == '\r' || *a == '/'))
            a++;
        const char *n = a;
        while (a < end && *a != '=' && *a != ' ' && *a != '>' && *a != '\t' && *a != '\n')
            a++;
        unsigned len = (unsigned)(a - n);
        while (a < end && (*a == ' ' || *a == '\t' || *a == '\n'))
            a++;
        const char *v = 0, *vend = 0;
        if (a < end && *a == '=') {
            a++;
            while (a < end && (*a == ' ' || *a == '\t' || *a == '\n'))
                a++;
            if (a < end && (*a == '"' || *a == '\'')) {
                char q = *a++;
                v = a;
                while (a < end && *a != q)
                    a++;
                vend = a;
                if (a < end)
                    a++;
            } else {
                v = a;
                while (a < end && *a != ' ' && *a != '>' && *a != '\t' && *a != '\n')
                    a++;
                vend = a;
            }
        }
        if (len == nl && v) {
            unsigned i = 0;
            for (; i < nl && lower(n[i]) == name[i]; i++)
                ;
            if (i == nl) {
                unsigned o = 0;
                while (v < vend && o + 1 < size) {
                    if (*v == '&') {
                        const char *e = v;
                        uint32_t cp = html_entity(&e, vend);
                        if (cp && cp < 128) {
                            out[o++] = (char)cp;
                            v = e;
                            continue;
                        }
                    }
                    if (*v != '\n' && *v != '\r' && *v != '\t')
                        out[o++] = *v;
                    v++;
                }
                out[o] = '\0';
                return 1;
            }
        }
        if (len == 0 && a < end)
            a++;
    }
    return 0;
}

static int is_tag(const char *tag, const char *const *list)
{
    for (; *list; list++)
        if (strcmp(tag, *list) == 0)
            return 1;
    return 0;
}

/* Obrazek: zapamietujemy adres, a w tekscie zostaje klocek z opisem (alt) */
static void add_image(struct parser *p, const char *src, const char *alt)
{
    struct html_doc *d = p->d;
    unsigned alen = (unsigned)strlen(alt);
    if (!src[0] || d->nimages >= 30000 ||
        !grow((void **)&d->images, &d->images_cap, d->nimages + 1, sizeof(char *)) ||
        !grow((void **)&d->text, &d->text_cap, d->text_len + alen + 1, 1)) {
        return;
    }
    char *copy = kmalloc(strlen(src) + 1);
    if (!copy)
        return;
    strcpy(copy, src);
    struct html_token *t = new_token(p, HT_IMAGE);
    if (!t) {
        kfree(copy);
        return;
    }
    d->images[d->nimages] = copy;
    t->img = (int16_t)d->nimages++;
    t->style = HS_IMAGE;
    t->space = (uint8_t)p->space;
    t->link = (int16_t)p->link;
    t->text = d->text_len;
    t->len = (uint16_t)alen;
    memcpy(d->text + d->text_len, alt, alen);
    d->text_len += alen;
    d->text[d->text_len] = '\0';
    p->space = 0;
}

static void open_link(struct parser *p, const char *href)
{
    struct html_doc *d = p->d;
    p->link = -1;
    if (!href[0] || d->nlinks >= 30000)
        return;
    if (!grow((void **)&d->links, &d->links_cap, d->nlinks + 1, sizeof(char *)))
        return;
    char *copy = kmalloc(strlen(href) + 1);
    if (!copy)
        return;
    strcpy(copy, href);
    d->links[d->nlinks] = copy;
    p->link = (int)d->nlinks++;
}

/* Obsluga znacznika. Zwraca, gdzie czytac dalej (np. za </script>). */
static const char *handle_tag(struct parser *p, const char *s, const char *end)
{
    static const char *const paragraphs[] = {
        "p", "ul", "ol", "table", "blockquote", "section", "article", "header", "footer",
        "nav", "main", "aside", "form", "center", "dl", "figure", "address", "fieldset", 0,
    };
    static const char *const lines[] = {
        "div", "br", "tr", "dt", "dd", "figcaption", "caption", "option", "summary", "details", 0,
    };
    static const char *const skipped[] = {
        "script", "style", "noscript", "svg", "template", "iframe", "object", "select", "math", 0,
    };

    const char *q = s + 1;
    if (q + 3 <= end && memcmp(q, "!--", 3) == 0) {          /* komentarz */
        for (q += 3; q + 3 <= end && memcmp(q, "-->", 3) != 0; q++)
            ;
        return q + 3 <= end ? q + 3 : end;
    }
    int closing = q < end && *q == '/';
    if (closing)
        q++;
    char tag[16];
    unsigned n = 0;
    while (q < end && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') || (*q >= '0' && *q <= '9'))) {
        if (n + 1 < sizeof(tag))
            tag[n++] = (char)lower(*q);
        q++;
    }
    tag[n] = '\0';
    if (!n && !closing && !(q < end && (*q == '!' || *q == '?'))) {
        put_char(p, '<');                                    /* samotny "<" w tekscie */
        return s + 1;
    }
    /* Koniec znacznika - ">" poza cudzyslowem */
    const char *attrs = q;
    char quote = 0;
    while (q < end && (quote || *q != '>')) {
        if (quote && *q == quote)
            quote = 0;
        else if (!quote && (*q == '"' || *q == '\''))
            quote = *q;
        q++;
    }
    const char *attrs_end = q;
    const char *next = q < end ? q + 1 : end;

    if (!closing && is_tag(tag, skipped)) {
        /* Kod programu, grafika wektorowa itp. - pomijamy az do </tag> */
        unsigned tl = (unsigned)strlen(tag);
        for (const char *r = next; r + tl + 2 <= end; r++)
            if (r[0] == '<' && r[1] == '/') {
                unsigned i = 0;
                while (i < tl && lower(r[2 + i]) == tag[i])
                    i++;
                if (i == tl) {
                    while (r < end && *r != '>')
                        r++;
                    return r < end ? r + 1 : end;
                }
            }
        return end;
    }

    if (strcmp(tag, "title") == 0) {
        p->in_title = !closing;
    } else if (strcmp(tag, "a") == 0) {
        flush_word(p);
        if (closing) {
            p->link = -1;
        } else {
            /* Linki w menu czesto stoja tuz obok siebie ("WszystkoWideoGrafika") -
             * jesli poprzednie slowo konczy sie litera, dajemy odstep */
            struct html_doc *d = p->d;
            if (d->ntok && d->tok[d->ntok - 1].type == HT_WORD) {
                char last = d->text[d->tok[d->ntok - 1].text + d->tok[d->ntok - 1].len - 1];
                if ((last >= 'a' && last <= 'z') || (last >= 'A' && last <= 'Z') || (last >= '0' && last <= '9'))
                    p->space = 1;
            }
            char href[600];
            if (html_attr(attrs, attrs_end, "href", href, sizeof(href)))
                open_link(p, href);
            else
                p->link = -1;
        }
    } else if (strcmp(tag, "i") == 0 || strcmp(tag, "em") == 0) {
        flush_word(p);
        p->italic += closing ? -1 : 1;
        if (p->italic < 0)
            p->italic = 0;
    } else if (strcmp(tag, "b") == 0 || strcmp(tag, "strong") == 0) {
        flush_word(p);
        p->bold += closing ? -1 : 1;
        if (p->bold < 0)
            p->bold = 0;
    } else if (n == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
        add_break(p, HT_PARA);
        p->heading = closing ? 0 : tag[1] == '1' ? 1 : tag[1] <= '3' ? 2 : 3;
    } else if (strcmp(tag, "li") == 0) {
        if (closing)
            add_break(p, HT_BREAK);
        else
            add_break(p, HT_BULLET);
    } else if (strcmp(tag, "hr") == 0) {
        add_break(p, HT_RULE);
    } else if (strcmp(tag, "pre") == 0) {
        add_break(p, HT_PARA);
        p->pre = !closing;
    } else if (strcmp(tag, "img") == 0) {
        char alt[80], src[600];
        if (!html_attr(attrs, attrs_end, "alt", alt, sizeof(alt)))
            alt[0] = '\0';
        /* Strony "leniwie ladujace" obrazki trzymaja prawdziwy adres w data-src */
        if (!html_attr(attrs, attrs_end, "data-src", src, sizeof(src)) &&
            !html_attr(attrs, attrs_end, "src", src, sizeof(src)))
            src[0] = '\0';
        flush_word(p);
        add_image(p, src, alt);
    } else if (strcmp(tag, "td") == 0 || strcmp(tag, "th") == 0) {
        flush_word(p);
        p->space = 1;
    } else if (is_tag(tag, paragraphs)) {
        add_break(p, HT_PARA);
    } else if (is_tag(tag, lines)) {
        if (strcmp(tag, "br") == 0) {
            flush_word(p);
            new_token(p, HT_BREAK);               /* <br><br> = pusta linia */
            p->space = 0;
        } else {
            add_break(p, HT_BREAK);
        }
    }
    return next;
}

struct html_doc *html_parse(const char *src, unsigned len, int is_html, int charset)
{
    struct html_doc *d = kzalloc(sizeof(*d));
    if (!d)
        return 0;
    struct parser p;
    memset(&p, 0, sizeof(p));
    p.d = d;
    p.ok = 1;
    p.link = -1;
    p.charset = charset;
    if (!is_html)
        p.pre = 1;                                /* zwykly tekst: linie jak w pliku */

    const char *s = src, *end = src + len, *breathe = src;
    while (s < end && p.ok) {
        if (s - breathe > 16384) {       /* duza strona - dajmy pulpitowi odetchnac */
            breathe = s;
            thread_yield();
        }
        if (is_html && *s == '<') {
            s = handle_tag(&p, s, end);
            continue;
        }
        if (is_html && *s == '&') {
            const char *e = s;
            uint32_t cp = html_entity(&e, end);
            if (cp) {
                put_codepoint(&p, cp);
                s = e;
                continue;
            }
        }
        if (*s == '\r') {
            s++;
            continue;
        }
        if (is_html && !p.pre && (*s == '\n' || *s == '\t')) {
            put_char(&p, ' ');
            s++;
            continue;
        }
        if (charset == HC_UTF8)
            put_codepoint(&p, utf8_next(&s, end));
        else
            put_codepoint(&p, html_legacy_char((uint8_t)*s++, charset));
    }
    flush_word(&p);
    if (!p.ok) {
        html_free(d);
        return 0;
    }
    /* Tytul bez spacji na koncu */
    unsigned tl = (unsigned)strlen(d->title);
    while (tl && d->title[tl - 1] == ' ')
        d->title[--tl] = '\0';
    return d;
}

void html_free(struct html_doc *d)
{
    if (!d)
        return;
    for (unsigned i = 0; i < d->nlinks; i++)
        kfree(d->links[i]);
    kfree(d->links);
    for (unsigned i = 0; i < d->nimages; i++)
        kfree(d->images[i]);
    kfree(d->images);
    kfree(d->tok);
    kfree(d->text);
    kfree(d);
}

char *html_to_plain(const struct html_doc *d, unsigned *out_len)
{
    unsigned cap = d->text_len + d->ntok * 4 + 16;
    char *out = kmalloc(cap);
    if (!out)
        return 0;
    unsigned n = 0;
    for (unsigned i = 0; i < d->ntok; i++) {
        const struct html_token *t = &d->tok[i];
        switch (t->type) {
        case HT_WORD:
            if (t->space && n && out[n - 1] != '\n')
                out[n++] = ' ';
            memcpy(out + n, d->text + t->text, t->len);
            n += t->len;
            break;
        case HT_BREAK:  out[n++] = '\n'; break;
        case HT_PARA:   out[n++] = '\n'; out[n++] = '\n'; break;
        case HT_BULLET: out[n++] = '\n'; out[n++] = '*'; out[n++] = ' '; break;
        case HT_RULE:   out[n++] = '\n'; out[n++] = '-'; out[n++] = '\n'; break;
        case HT_IMAGE:                               /* w terminalu: [opis obrazka] */
            if (!t->len)
                break;
            if (t->space && n && out[n - 1] != '\n')
                out[n++] = ' ';
            out[n++] = '[';
            memcpy(out + n, d->text + t->text, t->len);
            n += t->len;
            out[n++] = ']';
            break;
        }
    }
    out[n] = '\0';
    /* Terminal ma czcionke VGA - polskie litery zamieniamy na litery bez ogonkow */
    *out_len = utf8_to_ascii(out, n, out, cap);
    return out;
}

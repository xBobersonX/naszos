/* CSS - patrz css.h.
 *
 * Etapy:
 *  1. Rozbior arkusza: reguly "selektor { deklaracje }", bloki @media
 *     (sprawdzamy warunek, np. min-width, wzgledem szerokosci okna).
 *  2. Selektory: "div.menu > a#start" to ciag "zlozen" (znacznik, #id,
 *     .klasy, [atrybut], :first-child) polaczonych spacja (potomek) albo ">"
 *     (dziecko). Sprawdzamy je od prawej: najpierw sam element, potem
 *     rodzice. Czego nie rozumiemy (:hover, ::before, +, ~), tego regula nie
 *     dotyczy - lepiej pominac niz zle pokolorowac.
 *  3. Kaskada: pasujace reguly sortujemy wedlug zrodla (domyslne style
 *     przegladarki < styl strony), "sily" selektora i kolejnosci; na koncu
 *     atrybut style="" i !important.
 *  4. Wartosci: kolory, dlugosci (px, em, rem, %, pt), zmienne var(--x).
 *
 * Reguly trzymamy w "kubelkach" wedlug ostatniego elementu selektora (#id,
 * .klasa, znacznik) - element sprawdza tylko reguly, ktore moga go dotyczyc. */

#include "css.h"
#include "font.h"
#include "heap.h"
#include "string.h"
#include "thread.h"

#define MATCH_MAX 768
#define CLASSES_MAX 8

/* ---------- wlasnosci ---------- */

enum {
    P_UNKNOWN, P_CUSTOM, P_DISPLAY, P_COLOR, P_BACKGROUND, P_BACKGROUND_COLOR, P_FONT,
    P_FONT_WEIGHT, P_FONT_STYLE, P_FONT_SIZE, P_TEXT_ALIGN, P_TEXT_DECORATION, P_WHITE_SPACE,
    P_LIST_STYLE, P_LIST_STYLE_TYPE, P_VISIBILITY, P_FLOAT, P_CLEAR, P_WIDTH, P_HEIGHT,
    P_MAX_WIDTH, P_MIN_WIDTH, P_MARGIN, P_MARGIN_TOP, P_MARGIN_RIGHT, P_MARGIN_BOTTOM,
    P_MARGIN_LEFT, P_PADDING, P_PADDING_TOP, P_PADDING_RIGHT, P_PADDING_BOTTOM, P_PADDING_LEFT,
    P_BORDER, P_BORDER_TOP, P_BORDER_RIGHT, P_BORDER_BOTTOM, P_BORDER_LEFT, P_BORDER_WIDTH,
    P_BORDER_COLOR, P_BORDER_STYLE, P_OVERFLOW, P_POSITION, P_LINE_HEIGHT, P_BORDER_SPACING,
    P_MARGIN_INLINE_START, P_MARGIN_INLINE_END, P_PADDING_INLINE_START, P_PADDING_INLINE_END,
    P_TEXT_DECORATION_LINE, P_BOX_SIZING, P_BORDER_COLLAPSE, P_FLEX_DIRECTION, P_FLEX_WRAP,
    P_FLEX_FLOW, P_JUSTIFY_CONTENT, P_FLEX, P_FLEX_GROW, P_FLEX_SHRINK, P_FLEX_BASIS, P_GAP,
    P_COLUMN_GAP, P_TOP, P_RIGHT, P_BOTTOM, P_LEFT, P_INSET, P_OPACITY, P_TRANSFORM, P_COUNT,
};

static const char *const prop_names[P_COUNT] = {
    [P_DISPLAY] = "display", [P_COLOR] = "color", [P_BACKGROUND] = "background",
    [P_BACKGROUND_COLOR] = "background-color", [P_FONT] = "font", [P_FONT_WEIGHT] = "font-weight",
    [P_FONT_STYLE] = "font-style", [P_FONT_SIZE] = "font-size", [P_TEXT_ALIGN] = "text-align",
    [P_TEXT_DECORATION] = "text-decoration", [P_WHITE_SPACE] = "white-space",
    [P_LIST_STYLE] = "list-style", [P_LIST_STYLE_TYPE] = "list-style-type",
    [P_VISIBILITY] = "visibility", [P_FLOAT] = "float", [P_CLEAR] = "clear", [P_WIDTH] = "width",
    [P_HEIGHT] = "height", [P_MAX_WIDTH] = "max-width", [P_MIN_WIDTH] = "min-width",
    [P_MARGIN] = "margin", [P_MARGIN_TOP] = "margin-top", [P_MARGIN_RIGHT] = "margin-right",
    [P_MARGIN_BOTTOM] = "margin-bottom", [P_MARGIN_LEFT] = "margin-left", [P_PADDING] = "padding",
    [P_PADDING_TOP] = "padding-top", [P_PADDING_RIGHT] = "padding-right",
    [P_PADDING_BOTTOM] = "padding-bottom", [P_PADDING_LEFT] = "padding-left", [P_BORDER] = "border",
    [P_BORDER_TOP] = "border-top", [P_BORDER_RIGHT] = "border-right",
    [P_BORDER_BOTTOM] = "border-bottom", [P_BORDER_LEFT] = "border-left",
    [P_BORDER_WIDTH] = "border-width", [P_BORDER_COLOR] = "border-color",
    [P_BORDER_STYLE] = "border-style", [P_OVERFLOW] = "overflow", [P_POSITION] = "position",
    [P_LINE_HEIGHT] = "line-height", [P_BORDER_SPACING] = "border-spacing",
    [P_MARGIN_INLINE_START] = "margin-inline-start", [P_MARGIN_INLINE_END] = "margin-inline-end",
    [P_PADDING_INLINE_START] = "padding-inline-start", [P_PADDING_INLINE_END] = "padding-inline-end",
    [P_TEXT_DECORATION_LINE] = "text-decoration-line", [P_BOX_SIZING] = "box-sizing",
    [P_BORDER_COLLAPSE] = "border-collapse", [P_FLEX_DIRECTION] = "flex-direction",
    [P_FLEX_WRAP] = "flex-wrap", [P_FLEX_FLOW] = "flex-flow", [P_JUSTIFY_CONTENT] = "justify-content",
    [P_FLEX] = "flex", [P_FLEX_GROW] = "flex-grow", [P_FLEX_SHRINK] = "flex-shrink",
    [P_FLEX_BASIS] = "flex-basis", [P_GAP] = "gap", [P_COLUMN_GAP] = "column-gap",
    [P_TOP] = "top", [P_RIGHT] = "right", [P_BOTTOM] = "bottom", [P_LEFT] = "left",
    [P_INSET] = "inset", [P_OPACITY] = "opacity", [P_TRANSFORM] = "transform",
};

struct decl {
    uint8_t prop, important;
    const char *name;                    /* dla zmiennych --x */
    const char *value;
};

/* ---------- selektory ---------- */

struct attr_sel {
    const char *name, *value;            /* value NULL = sam atrybut wystarczy */
    char op;                             /* '=' rowny, '~' zawiera slowo, '^' zaczyna sie,
                                            '$' konczy sie, '*' zawiera, '|' rowny albo "wartosc-" */
};

struct compound {
    const char *tag;                     /* NULL = dowolny */
    const char *id;
    const char *cls[CLASSES_MAX];
    uint8_t ncls, nattr, first_child, last_child, link, root;
    struct attr_sel attr[3];
    uint8_t comb;                        /* jak laczy sie z poprzednim (lewym): 1 potomek, 2 dziecko,
                                            3 zaraz za bratem (+), 4 gdzies za bratem (~) */
    uint8_t empty, first_type, last_type, only_child;
    uint8_t nth;                         /* :nth-child(a n + b): 1, :nth-last-child: 2 */
    int16_t nth_a, nth_b;
    struct compound *not_[2];            /* :not(...) */
    uint8_t nnot;
};

struct rule {
    struct compound *parts;
    uint8_t nparts, origin;
    uint32_t spec, order;
    struct decl *decls;
    uint16_t ndecl;
};

/* Kubelki: reguly wedlug ostatniego zlozenia selektora */
struct bucket {
    const char *key;
    struct rule **rules;
    unsigned n, cap;
    struct bucket *next;
};

#define HASH 1024

struct css_ctx {
    struct arena mem;
    int viewport_w;
    uint8_t origin;                      /* 0 = domyslne style przegladarki, 1 = strona */
    uint32_t order;
    unsigned nrules;
    struct bucket *ids[HASH], *classes[HASH], *tags[HASH];
    struct bucket universal;
    /* Zmienne CSS (--kolor: ...) z regul dla :root / html / body */
    const char *var_name[256], *var_value[256];
    unsigned nvars;
    struct rule *matched[MATCH_MAX];
};

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int is_ident(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
           (unsigned char)c >= 0x80 || c == '\\';
}

static unsigned hash(const char *s, unsigned len)
{
    unsigned h = 5381;
    for (unsigned i = 0; i < len; i++)
        h = h * 33 + (unsigned char)s[i];
    return h % HASH;
}

static struct bucket *bucket_for(struct css_ctx *c, struct bucket **table, const char *key, int create)
{
    unsigned len = (unsigned)strlen(key), h = hash(key, len);
    for (struct bucket *b = table[h]; b; b = b->next)
        if (strcmp(b->key, key) == 0)
            return b;
    if (!create)
        return 0;
    struct bucket *b = arena_alloc(&c->mem, sizeof(*b));
    if (!b)
        return 0;
    b->key = key;
    b->next = table[h];
    table[h] = b;
    return b;
}

static void bucket_add(struct css_ctx *c, struct bucket *b, struct rule *r)
{
    if (b->n == b->cap) {
        unsigned cap = b->cap ? b->cap * 2 : 4;
        struct rule **nr = arena_alloc(&c->mem, sizeof(struct rule *) * cap);
        if (!nr)
            return;
        if (b->rules)
            memcpy(nr, b->rules, sizeof(struct rule *) * b->n);
        b->rules = nr;
        b->cap = cap;
    }
    b->rules[b->n++] = r;
}

/* Kopia fragmentu tekstu (malymi literami, jesli trzeba) */
static const char *dup(struct css_ctx *c, const char *s, unsigned n, int lowercase)
{
    char *d = arena_strndup(&c->mem, s, n);
    if (d && lowercase)
        for (char *p = d; *p; p++)
            *p = (char)lower(*p);
    return d;
}

static const char *skip_space(const char *s, const char *e)
{
    for (;;) {
        while (s < e && is_space(*s))
            s++;
        if (s + 1 < e && s[0] == '/' && s[1] == '*') {        /* komentarz */
            for (s += 2; s + 1 < e && !(s[0] == '*' && s[1] == '/'); s++)
                ;
            s = s + 2 <= e ? s + 2 : e;
            continue;
        }
        return s;
    }
}

/* :nth-child(odd), (even), (3), (2n+1), (-n+3) -> a, b. 0 = zly zapis */
static int parse_nth(const char *s, const char *e, int *a, int *b)
{
    s = skip_space(s, e);
    while (e > s && is_space(e[-1]))
        e--;
    if (e - s == 3 && memcmp(s, "odd", 3) == 0) { *a = 2; *b = 1; return 1; }
    if (e - s == 4 && memcmp(s, "even", 4) == 0) { *a = 2; *b = 0; return 1; }
    int sign = 1, num = 0, has = 0;
    if (s < e && (*s == '+' || *s == '-'))
        sign = *s++ == '-' ? -1 : 1;
    while (s < e && *s >= '0' && *s <= '9') {
        num = num * 10 + (*s++ - '0');
        has = 1;
    }
    if (s < e && (*s == 'n' || *s == 'N')) {
        *a = sign * (has ? num : 1);
        s = skip_space(s + 1, e);
        *b = 0;
        if (s < e) {
            int bs = *s == '-' ? -1 : 1;
            if (*s != '+' && *s != '-')
                return 0;
            s = skip_space(s + 1, e);
            int v = 0, any = 0;
            while (s < e && *s >= '0' && *s <= '9') {
                v = v * 10 + (*s++ - '0');
                any = 1;
            }
            if (!any)
                return 0;
            *b = bs * v;
        }
        return s == e || skip_space(s, e) == e;
    }
    if (!has)
        return 0;
    *a = 0;
    *b = sign * num;
    return s == e;
}

static int parse_compound(struct css_ctx *c, const char **ps, const char *e, struct compound *p,
                          uint32_t *a, uint32_t *b, uint32_t *cc, int nested)
{
    const char *s = *ps;
    int any = 0;
    while (s < e && !is_space(*s) && *s != '>' && *s != '+' && *s != '~' && *s != ',' && !(nested && *s == ')')) {
        if (*s == '*') {
            s++;
            any = 1;
        } else if (is_ident(*s)) {
            const char *t = s;
            while (s < e && is_ident(*s))
                s++;
            p->tag = dup(c, t, (unsigned)(s - t), 1);
            (*cc)++;
            any = 1;
        } else if (*s == '#' || *s == '.') {
            char kind = *s++;
            const char *t = s;
            while (s < e && is_ident(*s))
                s++;
            if (s == t)
                return 0;
            if (kind == '#') {
                p->id = dup(c, t, (unsigned)(s - t), 0);
                (*a)++;
            } else {
                if (p->ncls == CLASSES_MAX)
                    return 0;
                p->cls[p->ncls++] = dup(c, t, (unsigned)(s - t), 0);
                (*b)++;
            }
            any = 1;
        } else if (*s == '[') {                        /* [atrybut] albo [atrybut=wartosc] */
            const char *t = ++s;
            char q = 0;
            while (s < e && (q || *s != ']')) {
                if (q && *s == q)
                    q = 0;
                else if (!q && (*s == '"' || *s == '\''))
                    q = *s;
                s++;
            }
            if (s >= e || p->nattr == 3)
                return 0;
            const char *eq = t;
            while (eq < s && *eq != '=')
                eq++;
            char op = '=';
            const char *ne = eq;
            if (eq < s && eq > t && (eq[-1] == '~' || eq[-1] == '|' || eq[-1] == '^' ||
                                     eq[-1] == '$' || eq[-1] == '*')) {
                op = eq[-1];
                ne = eq - 1;
            }
            p->attr[p->nattr].op = op;
            while (ne > t && is_space(ne[-1]))
                ne--;
            p->attr[p->nattr].name = dup(c, t, (unsigned)(ne - t), 1);
            if (eq < s) {
                const char *v = eq + 1, *ve = s;
                while (v < ve && (is_space(*v) || *v == '"' || *v == '\''))
                    v++;
                while (ve > v && (is_space(ve[-1]) || ve[-1] == '"' || ve[-1] == '\'' ||
                                  ((ve[-1] == 'i' || ve[-1] == 's') && ve - 1 > v && is_space(ve[-2]))))
                    ve--;
                p->attr[p->nattr].value = dup(c, v, (unsigned)(ve - v), 0);
            }
            p->nattr++;
            s++;
            (*b)++;
            any = 1;
        } else if (*s == ':') {                        /* pseudoklasy */
            if (s + 1 < e && s[1] == ':')
                return 0;                               /* ::before itp. */
            const char *t = ++s;
            while (s < e && is_ident(*s))
                s++;
            unsigned l = (unsigned)(s - t);
            if (s < e && *s == '(') {
                const char *arg = ++s;
                int depth = 1;
                while (s < e && depth) {
                    if (*s == '(')
                        depth++;
                    else if (*s == ')')
                        depth--;
                    if (depth)
                        s++;
                }
                if (s >= e)
                    return 0;
                const char *arg_end = s++;
                if (l == 3 && memcmp(t, "not", 3) == 0) {
                    if (nested || p->nnot == 2)
                        return 0;
                    struct compound *np = arena_alloc(&c->mem, sizeof(*np));
                    if (!np)
                        return 0;
                    const char *q = skip_space(arg, arg_end);
                    if (!parse_compound(c, &q, arg_end, np, a, b, cc, 1))
                        return 0;
                    if (skip_space(q, arg_end) != arg_end)
                        return 0;                       /* :not(a b) - nie umiemy */
                    p->not_[p->nnot++] = np;
                } else if ((l == 9 && memcmp(t, "nth-child", 9) == 0) || (l == 14 && memcmp(t, "nth-last-child", 14) == 0)) {
                    int na, nb;
                    if (!parse_nth(arg, arg_end, &na, &nb))
                        return 0;
                    p->nth = l == 9 ? 1 : 2;
                    p->nth_a = (int16_t)na;
                    p->nth_b = (int16_t)nb;
                    (*b)++;
                } else {
                    return 0;                           /* :is(), :has()... */
                }
                any = 1;
                continue;
            }
            if (l == 4 && memcmp(t, "link", 4) == 0) p->link = 1;
            else if (l == 7 && memcmp(t, "visited", 7) == 0) return 0;   /* nie mamy historii */
            else if (l == 8 && memcmp(t, "any-link", 8) == 0) p->link = 1;
            else if (l == 11 && memcmp(t, "first-child", 11) == 0) p->first_child = 1;
            else if (l == 10 && memcmp(t, "last-child", 10) == 0) p->last_child = 1;
            else if (l == 10 && memcmp(t, "only-child", 10) == 0) p->only_child = 1;
            else if (l == 13 && memcmp(t, "first-of-type", 13) == 0) p->first_type = 1;
            else if (l == 12 && memcmp(t, "last-of-type", 12) == 0) p->last_type = 1;
            else if (l == 5 && memcmp(t, "empty", 5) == 0) p->empty = 1;
            else if (l == 4 && memcmp(t, "root", 4) == 0) p->root = 1;
            else
                return 0;                               /* :hover, :focus... - nigdy nie pasuja */
            (*b)++;
            any = 1;
        } else {
            return 0;
        }
    }
    *ps = s;
    return any;
}

/* Rozbior jednego selektora (bez przecinkow). 0 = nieobslugiwany */
static int parse_selector(struct css_ctx *c, const char *s, const char *e, struct rule *r)
{
    struct compound parts[16];
    int n = 0;
    uint32_t a = 0, b = 0, cc = 0;
    uint8_t comb = 0;
    s = skip_space(s, e);
    while (s < e) {
        if (n == 16)
            return 0;
        struct compound *p = &parts[n];
        memset(p, 0, sizeof(*p));
        p->comb = n ? comb : 0;
        if (!parse_compound(c, &s, e, p, &a, &b, &cc, 0))
            return 0;
        n++;
        s = skip_space(s, e);
        comb = 1;
        if (s < e && (*s == '>' || *s == '+' || *s == '~')) {
            comb = *s == '>' ? 2 : *s == '+' ? 3 : 4;
            s = skip_space(s + 1, e);
            if (s >= e)
                return 0;
        }
    }
    if (!n)
        return 0;
    r->parts = arena_alloc(&c->mem, sizeof(struct compound) * (unsigned)n);
    if (!r->parts)
        return 0;
    memcpy(r->parts, parts, sizeof(struct compound) * (unsigned)n);
    r->nparts = (uint8_t)n;
    r->spec = a << 16 | b << 8 | cc;
    return 1;
}

static int prop_id(const char *s, unsigned n)
{
    for (int i = 1; i < P_COUNT; i++)
        if (prop_names[i] && strlen(prop_names[i]) == n && memcmp(prop_names[i], s, n) == 0)
            return i;
    return P_UNKNOWN;
}

/* Deklaracje "wlasnosc: wartosc; ..." */
static unsigned parse_decls(struct css_ctx *c, const char *s, const char *e, struct decl **out)
{
    struct decl tmp[128];
    unsigned n = 0;
    while (s < e && n < 128) {
        s = skip_space(s, e);
        const char *ns = s;
        while (s < e && *s != ':' && *s != ';')
            s++;
        if (s >= e || *s == ';') {
            s++;
            continue;
        }
        const char *ne = s;
        while (ne > ns && is_space(ne[-1]))
            ne--;
        const char *v = skip_space(s + 1, e), *ve = v;
        int depth = 0;
        char quote = 0;
        while (ve < e && (depth || quote || *ve != ';')) {
            if (quote) {
                if (*ve == quote)
                    quote = 0;
            } else if (*ve == '"' || *ve == '\'') {
                quote = *ve;
            } else if (*ve == '(') {
                depth++;
            } else if (*ve == ')') {
                depth--;
            }
            ve++;
        }
        s = ve + 1;
        const char *vend = ve;
        while (vend > v && is_space(vend[-1]))
            vend--;
        int important = 0;
        if (vend - v >= 10 && memcmp(vend - 10, "!important", 10) == 0) {
            important = 1;
            vend -= 10;
            while (vend > v && is_space(vend[-1]))
                vend--;
        }
        unsigned nl = (unsigned)(ne - ns);
        struct decl *d = &tmp[n];
        memset(d, 0, sizeof(*d));
        if (nl > 2 && ns[0] == '-' && ns[1] == '-') {
            d->prop = P_CUSTOM;
            d->name = dup(c, ns, nl, 0);
        } else {
            char low[40];
            if (nl >= sizeof(low))
                continue;
            for (unsigned i = 0; i < nl; i++)
                low[i] = (char)lower(ns[i]);
            d->prop = (uint8_t)prop_id(low, nl);
            if (d->prop == P_UNKNOWN)
                continue;
        }
        d->important = (uint8_t)important;
        d->value = dup(c, v, (unsigned)(vend - v), 0);
        n++;
    }
    if (!n)
        return 0;
    *out = arena_alloc(&c->mem, sizeof(struct decl) * n);
    if (!*out)
        return 0;
    memcpy(*out, tmp, sizeof(struct decl) * n);
    return n;
}

/* Warunek @media, np. "screen and (min-width: 720px)" albo "print" */
static int media_ok(struct css_ctx *c, const char *s, const char *e)
{
    /* lista warunkow po przecinku - wystarczy jeden spelniony */
    while (s < e) {
        const char *q = s;
        int depth = 0;
        while (q < e && (depth || *q != ',')) {
            if (*q == '(') depth++;
            if (*q == ')') depth--;
            q++;
        }
        int ok = 1;
        const char *p = skip_space(s, q);
        if (q - p >= 3 && (memcmp(p, "not", 3) == 0 || memcmp(p, "NOT", 3) == 0))
            ok = 0;
        for (const char *t = p; t < q && ok; t++) {
            if (*t == 'p' && q - t >= 5 && memcmp(t, "print", 5) == 0)
                ok = 0;
            if (*t == 's' && q - t >= 6 && memcmp(t, "speech", 6) == 0)
                ok = 0;
            if (*t == '(') {
                const char *f = skip_space(t + 1, q), *fe = f;
                while (fe < q && *fe != ')')
                    fe++;
                const char *colon = f;
                while (colon < fe && *colon != ':')
                    colon++;
                unsigned nl = (unsigned)(colon - f);
                int num = 0;
                const char *v = skip_space(colon + 1, fe);
                while (v < fe && *v >= '0' && *v <= '9')
                    num = num * 10 + (*v++ - '0');
                if (v < fe && *v == '.')                    /* ulamek - pomijamy */
                    while (v < fe && ((*v >= '0' && *v <= '9') || *v == '.'))
                        v++;
                if (v + 2 <= fe && v[0] == 'e' && v[1] == 'm')
                    num *= 16;
                if (nl == 9 && memcmp(f, "min-width", 9) == 0)
                    ok = c->viewport_w >= num;
                else if (nl == 9 && memcmp(f, "max-width", 9) == 0)
                    ok = c->viewport_w <= num;
                else if (nl == 20 && memcmp(f, "prefers-color-scheme", 20) == 0)
                    ok = strstr_ci(colon, "light") && strstr_ci(colon, "light") < fe;
                else
                    ok = 0;                                 /* nieznana cecha - nie zakladamy */
                t = fe;
            }
        }
        if (ok)
            return 1;
        s = q + 1;
    }
    return 0;
}

/* Koniec bloku {...} z zagniezdzeniami */
static const char *block_end(const char *s, const char *e)
{
    int depth = 0;
    char quote = 0;
    for (; s < e; s++) {
        if (quote) {
            if (*s == '\\')
                s++;
            else if (*s == quote)
                quote = 0;
            continue;
        }
        if (*s == '"' || *s == '\'') quote = *s;
        else if (*s == '{') depth++;
        else if (*s == '}' && --depth == 0) return s;
    }
    return e;
}

static void add_rule(struct css_ctx *c, struct rule *r)
{
    struct compound *last = &r->parts[r->nparts - 1];
    struct bucket *b;
    if (last->id)
        b = bucket_for(c, c->ids, last->id, 1);
    else if (last->ncls)
        b = bucket_for(c, c->classes, last->cls[0], 1);
    else if (last->tag)
        b = bucket_for(c, c->tags, last->tag, 1);
    else
        b = &c->universal;
    if (b)
        bucket_add(c, b, r);
    c->nrules++;
}

static void parse_sheet(struct css_ctx *c, const char *s, const char *e)
{
    unsigned count = 0;
    while (s < e) {
        s = skip_space(s, e);
        if (s >= e)
            break;
        if (*s == '}') {
            s++;
            continue;
        }
        if (++count % 256 == 0)
            thread_yield();
        if (*s == '@') {
            const char *t = s + 1;
            while (t < e && is_ident(*t))
                t++;
            unsigned nl = (unsigned)(t - s - 1);
            const char *q = t;
            while (q < e && *q != '{' && *q != ';')
                q++;
            if (q >= e)
                break;
            if (*q == ';') {                                /* @import, @charset - pomijamy */
                s = q + 1;
                continue;
            }
            const char *be = block_end(q, e);
            if ((nl == 5 && memcmp(s + 1, "media", 5) == 0 && media_ok(c, t, q)) ||
                (nl == 8 && memcmp(s + 1, "supports", 8) == 0) ||
                (nl == 5 && memcmp(s + 1, "layer", 5) == 0))
                parse_sheet(c, q + 1, be);                  /* reguly w srodku */
            s = be < e ? be + 1 : e;
            continue;
        }
        const char *brace = s;
        while (brace < e && *brace != '{')
            brace++;
        if (brace >= e)
            break;
        const char *be = block_end(brace, e);
        struct decl *decls = 0;
        unsigned nd = parse_decls(c, brace + 1, be, &decls);
        if (nd) {
            /* Selektory po przecinku - kazdy to osobna regula z tymi samymi deklaracjami */
            const char *p = s;
            while (p < brace) {
                const char *q = p;
                int depth = 0;
                while (q < brace && (depth || *q != ',')) {
                    if (*q == '(' || *q == '[') depth++;
                    if (*q == ')' || *q == ']') depth--;
                    q++;
                }
                const char *qe = q;
                while (qe > p && is_space(qe[-1]))
                    qe--;
                struct rule *r = arena_alloc(&c->mem, sizeof(*r));
                if (r && parse_selector(c, p, qe, r)) {
                    r->decls = decls;
                    r->ndecl = (uint16_t)nd;
                    r->origin = c->origin;
                    r->order = c->order++;
                    add_rule(c, r);
                }
                p = q + 1;
            }
        }
        s = be < e ? be + 1 : e;
    }
}

/* ---------- domyslne style przegladarki ---------- */

static const char ua_sheet[] =
    "html,address,blockquote,body,dd,div,dl,dt,fieldset,form,h1,h2,h3,h4,h5,h6,ol,p,ul,center,"
    "hr,menu,pre,header,footer,nav,main,section,article,aside,figure,figcaption,details,summary,"
    "legend,noscript{display:block}"
    "head,script,style,title,meta,link,template,param,datalist,area,base,option,iframe,object,"
    "embed,video,audio,canvas,map,svg,input[type=hidden]{display:none}"
    "[hidden]{display:none}"
    "li{display:list-item}"
    "table{display:table;border-spacing:2px}"
    "tr{display:table-row}thead,tbody,tfoot{display:table-row-group}"
    "td,th{display:table-cell;padding:1px}"
    "caption{display:table-caption;text-align:center}"
    "body{margin:8px}"
    "h1{font-size:2em;margin:.67em 0;font-weight:bold}"
    "h2{font-size:1.5em;margin:.83em 0;font-weight:bold}"
    "h3{font-size:1.17em;margin:1em 0;font-weight:bold}"
    "h4,h5,h6{margin:1.33em 0;font-weight:bold}"
    "p,dl,figure{margin:1em 0}"
    "blockquote{margin:1em 40px}"
    "ul,ol{margin:1em 0;padding-left:40px}"
    "ul{list-style-type:disc}ol{list-style-type:decimal}"
    "li ul,li ol{margin:0}"
    "dd{margin-left:40px}"
    "b,strong,th{font-weight:bold}"
    "i,em,cite,var,dfn{font-style:italic}"
    "a:link{color:#0645ad;text-decoration:underline}"
    "pre{white-space:pre;margin:1em 0}"
    "center{text-align:center}th{text-align:center}"
    "hr{border-top:1px solid #a2a9b1;margin:.5em 0}"
    "small,sub,sup{font-size:smaller}"
    "u,ins{text-decoration:underline}"
    "s,strike,del{text-decoration:line-through}"
    "input,select,textarea,button,img{display:inline-block}"
    "input[type=submit],button{padding:2px 8px;border:1px solid #767676;background-color:#efefef}"
    "input,textarea{border:1px solid #767676;padding:2px}";

/* ---------- wartosci ---------- */

static const struct { const char *name; uint32_t rgb; } named_colors[] = {
    { "black", 0x000000 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 },
    { "blue", 0x0000FF }, { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xC0C0C0 },
    { "maroon", 0x800000 }, { "purple", 0x800080 }, { "fuchsia", 0xFF00FF }, { "lime", 0x00FF00 },
    { "olive", 0x808000 }, { "yellow", 0xFFFF00 }, { "navy", 0x000080 }, { "teal", 0x008080 },
    { "aqua", 0x00FFFF }, { "orange", 0xFFA500 }, { "darkgray", 0xA9A9A9 }, { "darkgrey", 0xA9A9A9 },
    { "lightgray", 0xD3D3D3 }, { "lightgrey", 0xD3D3D3 }, { "gold", 0xFFD700 }, { "pink", 0xFFC0CB },
    { "brown", 0xA52A2A }, { "darkblue", 0x00008B }, { "darkred", 0x8B0000 }, { "darkgreen", 0x006400 },
    { "whitesmoke", 0xF5F5F5 }, { "gainsboro", 0xDCDCDC }, { "lightblue", 0xADD8E6 },
    { "lightyellow", 0xFFFFE0 }, { "beige", 0xF5F5DC }, { "ivory", 0xFFFFF0 }, { "khaki", 0xF0E68C },
    { "crimson", 0xDC143C }, { "coral", 0xFF7F50 }, { "tomato", 0xFF6347 }, { "skyblue", 0x87CEEB },
    { "steelblue", 0x4682B4 }, { "royalblue", 0x4169E1 }, { "indigo", 0x4B0082 }, { "violet", 0xEE82EE },
};

static int hexv(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    ch = (char)lower(ch);
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    return -1;
}

/* Zamienia var(--x, zapas) na wartosc zmiennej */
static void resolve_vars(struct css_ctx *c, const char *v, char *out, unsigned size, int depth)
{
    unsigned n = 0;
    while (*v && n + 1 < size) {
        if (memcmp(v, "var(", 4) == 0) {
            const char *s = v + 4, *e = s;
            int paren = 1;
            while (*e && paren) {
                if (*e == '(') paren++;
                if (*e == ')') paren--;
                if (paren)
                    e++;
            }
            const char *comma = s;
            while (comma < e && *comma != ',')
                comma++;
            const char *ne = comma;
            while (ne > s && is_space(ne[-1]))
                ne--;
            while (s < ne && is_space(*s))
                s++;
            const char *val = 0;
            for (unsigned i = 0; i < c->nvars; i++)
                if (strlen(c->var_name[i]) == (unsigned)(ne - s) && memcmp(c->var_name[i], s, (size_t)(ne - s)) == 0)
                    val = c->var_value[i];
            char tmp[160];
            if (!val && comma < e) {                         /* zapas: var(--x, #fff) */
                unsigned l = (unsigned)(e - comma - 1);
                if (l >= sizeof(tmp))
                    l = sizeof(tmp) - 1;
                memcpy(tmp, comma + 1, l);
                tmp[l] = '\0';
                val = tmp;
            }
            if (val) {
                char inner[160];
                if (depth < 8)                                /* zmienna moze uzywac zmiennej (bez petli) */
                    resolve_vars(c, val, inner, sizeof(inner), depth + 1);
                else
                    inner[0] = '\0';
                for (const char *p = inner; *p && n + 1 < size; p++)
                    out[n++] = *p;
            }
            v = *e ? e + 1 : e;
            continue;
        }
        out[n++] = *v++;
    }
    out[n] = '\0';
}

/* Kolor po nazwie ("red", "cornflowerblue") - dla <canvas> (js_canvas.c). 0 = nieznana */
int css_named_color(const char *name, uint32_t *rgb)
{
    char word[24];
    unsigned n = 0;
    while (((*name >= 'a' && *name <= 'z') || (*name >= 'A' && *name <= 'Z')) && n + 1 < sizeof(word))
        word[n++] = (char)lower(*name++);
    word[n] = '\0';
    for (unsigned i = 0; i < sizeof(named_colors) / sizeof(named_colors[0]); i++)
        if (strcmp(word, named_colors[i].name) == 0) {
            *rgb = named_colors[i].rgb;
            return 1;
        }
    return 0;
}

/* Kolor: #abc, #aabbcc, rgb(1,2,3), rgba(...,0) = przezroczysty, nazwy. 0 = nieznany */
static int parse_color(const char *s, uint32_t *out, int *transparent)
{
    while (is_space(*s))
        s++;
    *transparent = 0;
    if (*s == '#') {
        int d[8], n = 0;
        for (s++; hexv(*s) >= 0 && n < 8; s++)
            d[n++] = hexv(*s);
        if (n == 3 || n == 4) {
            *out = (uint32_t)(d[0] * 17) << 16 | (uint32_t)(d[1] * 17) << 8 | (uint32_t)(d[2] * 17);
            *transparent = n == 4 && d[3] < 8;
            return 1;
        }
        if (n == 6 || n == 8) {
            *out = (uint32_t)(d[0] * 16 + d[1]) << 16 | (uint32_t)(d[2] * 16 + d[3]) << 8 | (uint32_t)(d[4] * 16 + d[5]);
            *transparent = n == 8 && d[6] < 8;
            return 1;
        }
        return 0;
    }
    if (memcmp(s, "rgb", 3) == 0) {
        const char *p = s + 3;
        if (*p == 'a')
            p++;
        if (*p != '(')
            return 0;
        p++;
        int v[4] = { 0, 0, 0, 255 }, n = 0;
        while (*p && *p != ')' && n < 4) {
            while (is_space(*p) || *p == ',' || *p == '/')
                p++;
            int num = 0, frac = 0, fd = 0, got = 0;
            while (*p >= '0' && *p <= '9') {
                num = num * 10 + (*p++ - '0');
                got = 1;
            }
            if (*p == '.') {
                for (p++; *p >= '0' && *p <= '9'; p++)
                    if (fd < 3) {
                        frac = frac * 10 + (*p - '0');
                        fd++;
                    }
                got = 1;
            }
            if (!got)
                break;
            if (*p == '%') {
                num = num * 255 / 100;
                p++;
            } else if (n == 3) {                             /* alfa 0..1 */
                while (fd < 3) {
                    frac *= 10;
                    fd++;
                }
                num = num * 255 + frac * 255 / 1000;
            }
            v[n++] = num > 255 ? 255 : num;
        }
        if (n < 3)
            return 0;
        *out = (uint32_t)v[0] << 16 | (uint32_t)v[1] << 8 | (uint32_t)v[2];
        *transparent = v[3] < 32;
        return 1;
    }
    char word[24];
    unsigned n = 0;
    while (((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')) && n + 1 < sizeof(word))
        word[n++] = (char)lower(*s++);
    word[n] = '\0';
    if (strcmp(word, "transparent") == 0) {
        *transparent = 1;
        *out = 0;
        return 1;
    }
    for (unsigned i = 0; i < sizeof(named_colors) / sizeof(named_colors[0]); i++)
        if (strcmp(word, named_colors[i].name) == 0) {
            *out = named_colors[i].rgb;
            return 1;
        }
    return 0;
}

/* Dlugosc: 12px, 1.5em, 2rem, 50%, 10pt, 0, auto. Wynik w px (albo %). 0 = nieznana */
static int parse_len(const char *s, int font_px, int viewport_w, struct css_len *out, const char **end)
{
    while (is_space(*s))
        s++;
    if (memcmp(s, "auto", 4) == 0) {
        out->u = L_AUTO;
        out->v = 0;
        if (end)
            *end = s + 4;
        return 1;
    }
    int neg = 0;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    int whole = 0, frac = 0, fd = 0, got = 0;
    while (*s >= '0' && *s <= '9') {
        whole = whole * 10 + (*s++ - '0');
        got = 1;
        if (whole > 100000)
            whole = 100000;
    }
    if (*s == '.')
        for (s++; *s >= '0' && *s <= '9'; s++, got = 1)
            if (fd < 3) {
                frac = frac * 10 + (*s - '0');
                fd++;
            }
    if (!got)
        return 0;
    while (fd < 3) {
        frac *= 10;
        fd++;
    }
    int64_t milli = (int64_t)whole * 1000 + frac;           /* wartosc * 1000 */
    int64_t px;
    out->u = L_PX;
    if (s[0] == 'p' && s[1] == 'x') { px = milli / 1000; s += 2; }
    else if (s[0] == 'e' && s[1] == 'm') { px = milli * font_px / 1000; s += 2; }
    else if (s[0] == 'r' && s[1] == 'e' && s[2] == 'm') { px = milli * 16 / 1000; s += 3; }
    else if (s[0] == 'e' && s[1] == 'x') { px = milli * font_px / 2000; s += 2; }
    else if (s[0] == 'p' && s[1] == 't') { px = milli * 4 / 3000; s += 2; }
    else if (s[0] == 'v' && s[1] == 'w') { px = milli * viewport_w / 100000; s += 2; }
    else if (s[0] == 'c' && s[1] == 'h') { px = milli * font_px / 2000; s += 2; }
    else if (*s == '%') { out->u = L_PCT; px = milli / 10; s++; }  /* procent * 100 */
    else if (milli == 0) { px = 0; }
    else return 0;                                          /* liczba bez jednostki - nie dlugosc */
    out->v = (int32_t)(neg ? -px : px);
    if (end)
        *end = s;
    return 1;
}

static unsigned strspn_digits(const char *s)
{
    unsigned n = 0;
    while ((s[n] >= '0' && s[n] <= '9') || s[n] == '.')
        n++;
    return n;
}

/* transform: translate(x, y) / translateX(x) / translateY(y) / translate3d(x, y, z) */
static void parse_translate(const char *v, int fpx, int vw, struct style *st)
{
    st->tx.u = st->ty.u = L_AUTO;
    st->tx.v = st->ty.v = 0;
    for (const char *p = v; *p; p++) {
        if (memcmp(p, "translate", 9) != 0)
            continue;
        const char *q = p + 9;
        int which = 0;                                   /* 0 = x i y, 1 = x, 2 = y */
        if (*q == 'X') { which = 1; q++; }
        else if (*q == 'Y') { which = 2; q++; }
        else if (memcmp(q, "3d", 2) == 0) q += 2;
        if (*q != '(')
            continue;
        q++;
        struct css_len l;
        const char *e;
        if (!parse_len(q, fpx, vw, &l, &e))
            return;
        if (which == 2) {
            st->ty = l;
        } else {
            st->tx = l;
            while (*e == ' ')
                e++;
            if (which == 0 && *e == ',' && parse_len(e + 1, fpx, vw, &l, &e))
                st->ty = l;
        }
        p = q;
    }
}

static int word_is(const char *s, const char *w)
{
    while (is_space(*s))
        s++;
    unsigned n = (unsigned)strlen(w);
    for (unsigned i = 0; i < n; i++)
        if (lower(s[i]) != w[i])
            return 0;
    return !is_ident(s[n]);
}

/* Kolejne "slowa" wartosci (np. "1px solid #ccc") */
static const char *next_token(const char *s, char *out, unsigned size)
{
    while (is_space(*s) || *s == ',')
        s++;
    unsigned n = 0;
    int depth = 0;
    while (*s && (depth || (!is_space(*s) && *s != ',')) && n + 1 < size) {
        if (*s == '(') depth++;
        if (*s == ')') depth--;
        out[n++] = *s++;
    }
    out[n] = '\0';
    return n ? s : 0;
}

static void set_box4(struct css_len box[4], const char *v, int font_px, int vw)
{
    struct css_len vals[4];
    int n = 0;
    const char *e;
    while (n < 4 && parse_len(v, font_px, vw, &vals[n], &e)) {
        n++;
        v = e;
    }
    if (!n)
        return;
    box[0] = vals[0];
    box[1] = n > 1 ? vals[1] : vals[0];
    box[2] = n > 2 ? vals[2] : vals[0];
    box[3] = n > 3 ? vals[3] : box[1];
}

/* border: szerokosc styl kolor (w dowolnej kolejnosci) */
static void set_border(struct style *st, int side, const char *v, int font_px, int vw)
{
    char tok[64];
    int width = 3, has_style = 0, none = 0;
    uint32_t color = st->color;
    const char *p = v;
    while ((p = next_token(p, tok, sizeof(tok))) != 0) {
        struct css_len l;
        uint32_t c;
        int tr;
        if (parse_len(tok, font_px, vw, &l, 0) && l.u == L_PX)
            width = l.v;
        else if (strcmp(tok, "thin") == 0) width = 1;
        else if (strcmp(tok, "medium") == 0) width = 3;
        else if (strcmp(tok, "thick") == 0) width = 5;
        else if (strcmp(tok, "none") == 0 || strcmp(tok, "hidden") == 0) none = 1;
        else if (strcmp(tok, "solid") == 0 || strcmp(tok, "dotted") == 0 || strcmp(tok, "dashed") == 0 ||
                 strcmp(tok, "double") == 0 || strcmp(tok, "groove") == 0 || strcmp(tok, "ridge") == 0 ||
                 strcmp(tok, "inset") == 0 || strcmp(tok, "outset") == 0) has_style = 1;
        else if (parse_color(tok, &c, &tr)) {
            color = c;
            if (tr)
                none = 1;
        }
    }
    int w = (has_style && !none) ? width : 0;
    for (int i = 0; i < 4; i++)
        if (side < 0 || side == i) {
            st->border[i] = (int16_t)(w > 50 ? 50 : w);
            st->border_color[i] = color;
        }
}

static int font_size_value(const char *v, int parent_px, int vw)
{
    static const struct { const char *name; int px; } keywords[] = {
        { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 }, { "large", 18 },
        { "x-large", 24 }, { "xx-large", 32 },
    };
    for (unsigned i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++)
        if (word_is(v, keywords[i].name))
            return keywords[i].px;
    if (word_is(v, "smaller"))
        return parent_px * 5 / 6;
    if (word_is(v, "larger"))
        return parent_px * 6 / 5;
    struct css_len l;
    if (!parse_len(v, parent_px, vw, &l, 0) || l.u == L_AUTO)
        return 0;
    if (l.u == L_PCT)
        return parent_px * l.v / 10000;
    return l.v;
}

/* Jedna deklaracja -> styl */
static void apply(struct css_ctx *c, struct style *st, const struct style *parent, const struct decl *d)
{
    char v[256];
    resolve_vars(c, d->value, v, sizeof(v), 0);
    int vw = c->viewport_w, fpx = st->font_px;
    uint32_t col;
    int tr;
    struct css_len l;
    if (word_is(v, "inherit") && parent) {
        switch (d->prop) {
        case P_COLOR: st->color = parent->color; break;
        case P_FONT_SIZE: st->font_px = parent->font_px; break;
        case P_BACKGROUND: case P_BACKGROUND_COLOR: st->bg = parent->bg; st->has_bg = parent->has_bg; break;
        }
        return;
    }
    switch (d->prop) {
    case P_DISPLAY:
        if (word_is(v, "none")) st->display = D_NONE;
        else if (word_is(v, "flex"))
            st->display = D_FLEX;
        else if (word_is(v, "block") || word_is(v, "grid") || word_is(v, "flow-root"))
            st->display = D_BLOCK;
        else if (word_is(v, "inline")) st->display = D_INLINE;
        else if (word_is(v, "inline-block") || word_is(v, "inline-flex") || word_is(v, "inline-grid"))
            st->display = D_INLINE_BLOCK;
        else if (word_is(v, "list-item")) st->display = D_LIST_ITEM;
        else if (word_is(v, "table")) st->display = D_TABLE;
        else if (word_is(v, "inline-table")) st->display = D_TABLE;
        else if (word_is(v, "table-row")) st->display = D_ROW;
        else if (word_is(v, "table-cell")) st->display = D_CELL;
        else if (word_is(v, "table-row-group") || word_is(v, "table-header-group") ||
                 word_is(v, "table-footer-group")) st->display = D_ROW_GROUP;
        else if (word_is(v, "table-caption")) st->display = D_CAPTION;
        else if (word_is(v, "contents")) st->display = D_INLINE;
        break;
    case P_COLOR:
        if (parse_color(v, &col, &tr) && !tr)
            st->color = col;
        break;
    case P_BACKGROUND:
    case P_BACKGROUND_COLOR: {
        char tok[64];
        const char *p = v;
        if (word_is(v, "none")) {
            st->has_bg = 0;
            break;
        }
        while ((p = next_token(p, tok, sizeof(tok))) != 0)
            if (parse_color(tok, &col, &tr)) {
                st->has_bg = !tr;
                st->bg = col;
            }
        break;
    }
    case P_FONT_WEIGHT:
        if (word_is(v, "bold") || word_is(v, "bolder")) st->bold = 1;
        else if (word_is(v, "normal") || word_is(v, "lighter")) st->bold = 0;
        else if (v[0] >= '1' && v[0] <= '9') st->bold = v[0] >= '6';
        break;
    case P_FONT_STYLE:
        st->italic = word_is(v, "italic") || word_is(v, "oblique");
        break;
    case P_FONT_SIZE: {
        int px = font_size_value(v, parent ? parent->font_px : 16, vw);
        if (px > 0)
            st->font_px = (int16_t)(px > 96 ? 96 : px);
        break;
    }
    case P_FONT: {                                        /* skrot: italic bold 14px/1.5 Arial */
        char tok[64];
        const char *p = v;
        while ((p = next_token(p, tok, sizeof(tok))) != 0) {
            if (strcmp(tok, "italic") == 0) st->italic = 1;
            else if (strcmp(tok, "bold") == 0 || (tok[0] >= '6' && tok[0] <= '9' && tok[1] == '0'))
                st->bold = 1;
            else if ((tok[0] >= '0' && tok[0] <= '9') || tok[0] == '.') {
                char *slash = tok;
                while (*slash && *slash != '/')
                    slash++;
                *slash = '\0';
                int px = font_size_value(tok, parent ? parent->font_px : 16, vw);
                if (px > 0)
                    st->font_px = (int16_t)px;
            }
        }
        break;
    }
    case P_TEXT_ALIGN:
        if (word_is(v, "center")) st->align = AL_CENTER;
        else if (word_is(v, "right") || word_is(v, "end")) st->align = AL_RIGHT;
        else st->align = AL_LEFT;
        break;
    case P_TEXT_DECORATION:
    case P_TEXT_DECORATION_LINE:
        /* 1 = podkreslenie, 2 = przekreslenie (line-through) */
        st->underline = strstr_ci(v, "underline") ? 1 : strstr_ci(v, "line-through") ? 2 : 0;
        break;
    case P_WHITE_SPACE:
        st->white_space = word_is(v, "nowrap") ? WS_NOWRAP : word_is(v, "pre-line") ? WS_PRE_LINE :
                          (word_is(v, "pre") || word_is(v, "pre-wrap") || word_is(v, "break-spaces")) ? WS_PRE : WS_NORMAL;
        break;
    case P_LIST_STYLE:
    case P_LIST_STYLE_TYPE:
        if (strstr_ci(v, "none")) st->list = LS_NONE;
        else if (strstr_ci(v, "decimal")) st->list = LS_DECIMAL;
        else if (strstr_ci(v, "circle")) st->list = LS_CIRCLE;
        else if (strstr_ci(v, "square")) st->list = LS_SQUARE;
        else if (strstr_ci(v, "disc")) st->list = LS_DISC;
        break;
    case P_VISIBILITY:
        st->hidden = word_is(v, "hidden") || word_is(v, "collapse");
        break;
    case P_FLOAT:
        st->floating = word_is(v, "left") ? FL_LEFT : word_is(v, "right") ? FL_RIGHT : FL_NONE;
        break;
    case P_CLEAR:
        st->clear = word_is(v, "left") ? CL_LEFT : word_is(v, "right") ? CL_RIGHT :
                    word_is(v, "both") ? CL_BOTH : CL_NONE;
        break;
    case P_WIDTH:      if (parse_len(v, fpx, vw, &l, 0)) st->width = l; break;
    case P_HEIGHT:     if (parse_len(v, fpx, vw, &l, 0)) st->height = l; break;
    case P_MAX_WIDTH:  if (word_is(v, "none")) st->max_width.u = L_AUTO;
                       else if (parse_len(v, fpx, vw, &l, 0)) st->max_width = l; break;
    case P_MIN_WIDTH:  if (parse_len(v, fpx, vw, &l, 0)) st->min_width = l; break;
    case P_MARGIN:     set_box4(st->margin, v, fpx, vw); break;
    case P_PADDING:    set_box4(st->padding, v, fpx, vw); break;
    case P_MARGIN_TOP: case P_MARGIN_RIGHT: case P_MARGIN_BOTTOM: case P_MARGIN_LEFT:
        if (parse_len(v, fpx, vw, &l, 0)) st->margin[d->prop - P_MARGIN_TOP] = l;
        break;
    case P_MARGIN_INLINE_START: if (parse_len(v, fpx, vw, &l, 0)) st->margin[3] = l; break;
    case P_MARGIN_INLINE_END:   if (parse_len(v, fpx, vw, &l, 0)) st->margin[1] = l; break;
    case P_PADDING_TOP: case P_PADDING_RIGHT: case P_PADDING_BOTTOM: case P_PADDING_LEFT:
        if (parse_len(v, fpx, vw, &l, 0) && l.v >= 0) st->padding[d->prop - P_PADDING_TOP] = l;
        break;
    case P_PADDING_INLINE_START: if (parse_len(v, fpx, vw, &l, 0)) st->padding[3] = l; break;
    case P_PADDING_INLINE_END:   if (parse_len(v, fpx, vw, &l, 0)) st->padding[1] = l; break;
    case P_BORDER:        set_border(st, -1, v, fpx, vw); break;
    case P_BORDER_TOP:    set_border(st, 0, v, fpx, vw); break;
    case P_BORDER_RIGHT:  set_border(st, 1, v, fpx, vw); break;
    case P_BORDER_BOTTOM: set_border(st, 2, v, fpx, vw); break;
    case P_BORDER_LEFT:   set_border(st, 3, v, fpx, vw); break;
    case P_BORDER_WIDTH: {
        struct css_len b4[4];
        memset(b4, 0, sizeof(b4));
        set_box4(b4, v, fpx, vw);
        for (int i = 0; i < 4; i++)                        /* szerokosc liczy sie tylko przy stylu ramki */
            if (st->border[i])
                st->border[i] = (int16_t)b4[i].v;
        break;
    }
    case P_BORDER_COLOR:
        if (parse_color(v, &col, &tr))
            for (int i = 0; i < 4; i++)
                st->border_color[i] = col;
        break;
    case P_BORDER_STYLE:
        if (word_is(v, "none") || word_is(v, "hidden"))
            for (int i = 0; i < 4; i++)
                st->border[i] = 0;
        break;
    case P_OVERFLOW:
        st->overflow = !word_is(v, "visible");
        break;
    case P_POSITION:
        /* absolute/fixed: element "wyjety" z biegu strony - uklad stawia go
         * wzgledem najblizszego przodka z position (layout.c) */
        st->position = word_is(v, "relative") || word_is(v, "sticky") ? POS_RELATIVE :
                       word_is(v, "absolute") ? POS_ABSOLUTE : word_is(v, "fixed") ? POS_FIXED : POS_STATIC;
        st->out_of_flow = st->position == POS_ABSOLUTE || st->position == POS_FIXED;
        break;
    case P_TOP: case P_RIGHT: case P_BOTTOM: case P_LEFT:
        if (parse_len(v, fpx, vw, &l, 0))
            st->inset[d->prop - P_TOP] = l;
        break;
    case P_INSET:
        set_box4(st->inset, v, fpx, vw);
        break;
    case P_OPACITY: {
        /* opacity: 0 - element niewidoczny (np. ukryte menu); innej przezroczystosci nie rysujemy */
        const char *q = v;
        while (*q == ' ')
            q++;
        int zero = (*q == '0' || *q == '.') && q[strspn_digits(q)] != '%';
        for (const char *z = q; *z && *z != ' ' && *z != ';' && *z != '!'; z++)
            if (*z >= '1' && *z <= '9')
                zero = 0;
        if (zero)
            st->hidden = 1;
        else                                       /* opacity: 1 odkrywa to, co ukryla wczesniejsza regula */
            st->hidden = parent ? parent->hidden : 0;
        break;
    }
    case P_TRANSFORM:
        parse_translate(v, fpx, vw, st);
        break;
    case P_LINE_HEIGHT:
        if (word_is(v, "normal")) {
            st->line_px = 0;
        } else if (parse_len(v, fpx, vw, &l, 0)) {
            st->line_px = (int16_t)(l.u == L_PCT ? fpx * l.v / 10000 : l.v);
        } else {                                           /* sama liczba: razy rozmiar czcionki */
            struct css_len em;
            char tmp[64];
            ksnprintf(tmp, sizeof(tmp), "%sem", v);
            if (parse_len(tmp, fpx, vw, &em, 0))
                st->line_px = (int16_t)em.v;
        }
        break;
    case P_FLEX_DIRECTION:
        st->flex_column = strstr_ci(v, "column") != 0;
        break;
    case P_FLEX_WRAP:
        st->flex_wrap = word_is(v, "wrap") || word_is(v, "wrap-reverse");
        break;
    case P_FLEX_FLOW:
        st->flex_column = strstr_ci(v, "column") != 0;
        st->flex_wrap = strstr_ci(v, "wrap") && !strstr_ci(v, "nowrap");
        break;
    case P_JUSTIFY_CONTENT:
        st->justify = word_is(v, "center") ? JU_CENTER : (word_is(v, "flex-end") || word_is(v, "end") ||
                      word_is(v, "right")) ? JU_END : word_is(v, "space-between") ? JU_BETWEEN :
                      (word_is(v, "space-around") || word_is(v, "space-evenly")) ? JU_AROUND : JU_START;
        break;
    case P_FLEX: {                                        /* flex: 1  /  flex: 0 0 200px  /  flex: none */
        if (word_is(v, "none")) {
            st->flex_grow = 0;
            st->flex_shrink_none = 1;
            break;
        }
        if (word_is(v, "auto")) {
            st->flex_grow = 1;
            break;
        }
        char tok[32];
        const char *p = v;
        int n = 0;
        while ((p = next_token(p, tok, sizeof(tok))) != 0) {
            if (parse_len(tok, fpx, vw, &l, 0) && (l.u != L_PX || l.v != 0 || n >= 2)) {
                st->flex_basis = l;
            } else if (n == 0) {
                st->flex_grow = (uint8_t)(tok[0] >= '1' && tok[0] <= '9' ? tok[0] - '0' : 0);
                st->flex_basis.u = L_PX;             /* "flex: 1" = podstawa 0 */
                st->flex_basis.v = 0;
            } else if (n == 1) {
                st->flex_shrink_none = tok[0] == '0';
            }
            n++;
        }
        break;
    }
    case P_FLEX_GROW:
        st->flex_grow = (uint8_t)(v[0] >= '1' && v[0] <= '9' ? v[0] - '0' : 0);
        break;
    case P_FLEX_SHRINK:
        st->flex_shrink_none = v[0] == '0';
        break;
    case P_FLEX_BASIS:
        if (parse_len(v, fpx, vw, &l, 0))
            st->flex_basis = l;
        break;
    case P_GAP:
    case P_COLUMN_GAP: {
        const char *e = v;
        if (parse_len(v, fpx, vw, &l, &e) && l.u == L_PX) {
            struct css_len l2;
            st->gap = (int16_t)(d->prop == P_GAP && parse_len(e, fpx, vw, &l2, 0) && l2.u == L_PX ? l2.v : l.v);
        }
        break;
    }
    case P_BOX_SIZING:
        st->border_box = word_is(v, "border-box");
        break;
    case P_BORDER_COLLAPSE:
        if (word_is(v, "collapse"))
            st->border_spacing = 0;
        break;
    case P_BORDER_SPACING:
        if (parse_len(v, fpx, vw, &l, 0) && l.u == L_PX)
            st->border_spacing = (int16_t)l.v;
        break;
    }
}

/* ---------- dopasowanie ---------- */

static int elem(const struct dom_node *n) { return n && n->type == N_ELEMENT && n->name[0] != '#'; }

static const struct dom_node *prev_elem(const struct dom_node *n)
{
    for (n = n->prev; n; n = n->prev)
        if (n->type == N_ELEMENT)
            return n;
    return 0;
}

static const struct dom_node *next_elem(const struct dom_node *n)
{
    for (n = n->next; n; n = n->next)
        if (n->type == N_ELEMENT)
            return n;
    return 0;
}

static int match_compound(const struct compound *p, const struct dom_node *n)
{
    if (!elem(n))
        return 0;
    if (p->tag && strcmp(p->tag, n->name) != 0)
        return 0;
    if (p->id && (!n->id || strcmp(p->id, n->id) != 0))
        return 0;
    for (int i = 0; i < p->ncls; i++)
        if (!dom_has_class(n, p->cls[i], (unsigned)strlen(p->cls[i])))
            return 0;
    for (int i = 0; i < p->nattr; i++) {
        const char *v = dom_attr(n, p->attr[i].name), *want = p->attr[i].value;
        if (!v)
            return 0;
        if (!want)
            continue;
        unsigned vl = (unsigned)strlen(v), wl = (unsigned)strlen(want);
        int ok;
        switch (p->attr[i].op) {
        case '^': ok = vl >= wl && memcmp(v, want, wl) == 0; break;
        case '$': ok = vl >= wl && memcmp(v + vl - wl, want, wl) == 0; break;
        case '*': ok = strstr_ci(v, want) != 0; break;
        case '|': ok = strcmp(v, want) == 0 || (vl > wl && memcmp(v, want, wl) == 0 && v[wl] == '-'); break;
        case '~': {                                      /* jedno ze slow oddzielonych spacja */
            ok = 0;
            for (const char *w = v; *w && !ok;) {
                while (*w == ' ')
                    w++;
                const char *e = w;
                while (*e && *e != ' ')
                    e++;
                ok = (unsigned)(e - w) == wl && memcmp(w, want, wl) == 0;
                w = e;
            }
            break;
        }
        default: ok = strcmp(v, want) == 0;
        }
        if (!ok)
            return 0;
    }
    if (p->link && !(n->tag == T_A && dom_attr(n, "href")))
        return 0;
    if ((p->first_child || p->only_child) && prev_elem(n))
        return 0;
    if ((p->last_child || p->only_child) && next_elem(n))
        return 0;
    if (p->root && n->tag != T_HTML)
        return 0;
    if (p->empty)
        for (const struct dom_node *ch = n->first; ch; ch = ch->next)
            if (ch->type == N_ELEMENT || ch->text_len)
                return 0;
    if (p->first_type || p->last_type) {
        for (const struct dom_node *q = p->first_type ? n->prev : n->next; q; q = p->first_type ? q->prev : q->next)
            if (q->type == N_ELEMENT && strcmp(q->name, n->name) == 0)
                return 0;
    }
    if (p->nth) {
        int pos = 1;                                       /* numer wsrod rodzenstwa (od 1) */
        for (const struct dom_node *q = p->nth == 1 ? prev_elem(n) : next_elem(n); q;
             q = p->nth == 1 ? prev_elem(q) : next_elem(q))
            pos++;
        int d = pos - p->nth_b;
        if (p->nth_a == 0) {
            if (d != 0)
                return 0;
        } else if (d % p->nth_a != 0 || d / p->nth_a < 0) {
            return 0;
        }
    }
    for (int i = 0; i < p->nnot; i++)
        if (match_compound(p->not_[i], n))
            return 0;
    return 1;
}

static int match(const struct rule *r, int idx, const struct dom_node *n)
{
    if (!match_compound(&r->parts[idx], n))
        return 0;
    if (idx == 0)
        return 1;
    switch (r->parts[idx].comb) {
    case 2:
        return match(r, idx - 1, n->parent);
    case 3: {
        const struct dom_node *p = prev_elem(n);
        return p && match(r, idx - 1, p);
    }
    case 4:
        for (const struct dom_node *p = prev_elem(n); p; p = prev_elem(p))
            if (match(r, idx - 1, p))
                return 1;
        return 0;
    }
    for (const struct dom_node *p = n->parent; elem(p); p = p->parent)
        if (match(r, idx - 1, p))
            return 1;
    return 0;
}

static unsigned collect(struct css_ctx *c, struct bucket *b, const struct dom_node *n, unsigned count)
{
    for (unsigned i = 0; b && i < b->n && count < MATCH_MAX; i++)
        if (match(b->rules[i], b->rules[i]->nparts - 1, n))
            c->matched[count++] = b->rules[i];
    return count;
}

static int rule_before(const struct rule *a, const struct rule *b)
{
    if (a->origin != b->origin) return a->origin < b->origin;
    if (a->spec != b->spec) return a->spec < b->spec;
    return a->order < b->order;
}

/* ---------- obliczanie stylu ---------- */

static void init_style(struct style *st, const struct style *parent)
{
    memset(st, 0, sizeof(*st));
    st->display = D_INLINE;
    st->color = 0x000000;
    st->font_px = 16;
    for (int i = 0; i < 4; i++) {
        st->border_color[i] = 0;
        st->margin[i].u = L_PX;                    /* domyslnie margines 0, nie "auto" */
    }
    if (parent) {                                         /* wlasnosci dziedziczone */
        st->color = parent->color;
        st->font_px = parent->font_px;
        st->bold = parent->bold;
        st->italic = parent->italic;
        st->align = parent->align;
        st->list = parent->list;
        st->white_space = parent->white_space;
        st->hidden = parent->hidden;
        st->underline = parent->underline;
        st->line_px = parent->line_px;
        st->border_spacing = parent->border_spacing;
    }
    for (int i = 0; i < 4; i++)
        st->border_color[i] = st->color;
}

static void pick_font(struct style *st)
{
    if (st->font_px >= 24)
        st->font = FONT_H1;
    else if (st->font_px >= 18 && st->bold)
        st->font = FONT_H2;
    else if (st->font_px <= 12)
        st->font = FONT_SMALL;
    else if (st->bold)
        st->font = FONT_BOLD;
    else if (st->italic)
        st->font = FONT_ITALIC;
    else
        st->font = FONT_REGULAR;
}

/* Deklaracje z atrybutu style="" (i stare atrybuty HTML: bgcolor, align, width) */
static void apply_inline(struct css_ctx *c, struct style *st, const struct style *parent,
                         const struct dom_node *n, int important)
{
    const char *s = dom_attr(n, "style");
    if (!s)
        return;
    struct decl *decls;
    unsigned nd = parse_decls(c, s, s + strlen(s), &decls);
    for (unsigned i = 0; i < nd; i++)
        if (decls[i].important == important && decls[i].prop != P_CUSTOM)
            apply(c, st, parent, &decls[i]);
}

static void presentational(struct css_ctx *c, struct style *st, const struct dom_node *n)
{
    const char *v;
    uint32_t col;
    int tr;
    struct css_len l;
    if ((v = dom_attr(n, "bgcolor")) && parse_color(v, &col, &tr)) {
        st->bg = col;
        st->has_bg = !tr;
    }
    if (n->tag == T_FONT && (v = dom_attr(n, "color")) && parse_color(v, &col, &tr))
        st->color = col;
    if ((v = dom_attr(n, "align")) && n->tag != T_IMG) {
        if (word_is(v, "center")) st->align = AL_CENTER;
        else if (word_is(v, "right")) st->align = AL_RIGHT;
    }
    if (n->tag == T_IMG && (v = dom_attr(n, "align"))) {
        if (word_is(v, "left")) st->floating = FL_LEFT;
        else if (word_is(v, "right")) st->floating = FL_RIGHT;
    }
    if ((v = dom_attr(n, "width")) && n->tag != T_IMG && n->tag != T_CANVAS) {   /* plotno: rozmiar pikseli, nie CSS */
        char tmp[32];
        ksnprintf(tmp, sizeof(tmp), "%s%s", v, v[strlen(v) - 1] == '%' ? "" : "px");
        if (parse_len(tmp, st->font_px, c->viewport_w, &l, 0))
            st->width = l;
    }
    if (n->tag == T_TABLE && (v = dom_attr(n, "border")) && v[0] != '0') {
        for (int i = 0; i < 4; i++) {
            st->border[i] = 1;
            st->border_color[i] = 0x808080;
        }
    }
    if (n->tag == T_TABLE && (v = dom_attr(n, "cellpadding")) && parse_len(v, 16, 0, &l, 0))
        (void)l;
}

static void compute(struct css_ctx *c, struct arena *mem, struct dom_node *n, const struct style *parent, unsigned *counter)
{
    if (++*counter % 64 == 0)
        thread_yield();
    /* Po zmianie strony przez JavaScript liczymy style jeszcze raz - element
     * ma juz swoja strukture, wiec ja nadpisujemy zamiast brac nowa */
    struct style *st = n->style ? n->style : arena_alloc(mem, sizeof(*st));
    if (!st)
        return;
    init_style(st, parent);
    n->style = st;

    unsigned count = 0;
    if (elem(n)) {
        if (n->id)
            count = collect(c, bucket_for(c, c->ids, n->id, 0), n, count);
        for (const char *s = n->cls; s && *s;) {        /* kazda klasa elementu */
            while (*s == ' ' || *s == '\t' || *s == '\n')
                s++;
            const char *w = s;
            while (*s && *s != ' ' && *s != '\t' && *s != '\n')
                s++;
            if (s > w) {
                char key[64];
                unsigned l = (unsigned)(s - w) < sizeof(key) - 1 ? (unsigned)(s - w) : sizeof(key) - 1;
                memcpy(key, w, l);
                key[l] = '\0';
                count = collect(c, bucket_for(c, c->classes, key, 0), n, count);
            }
        }
        count = collect(c, bucket_for(c, c->tags, n->name, 0), n, count);
        count = collect(c, &c->universal, n, count);
    }
    /* Sortowanie przez wstawianie - pasujacych regul jest zwykle kilkanascie */
    for (unsigned i = 1; i < count; i++) {
        struct rule *r = c->matched[i];
        unsigned j = i;
        while (j > 0 && rule_before(r, c->matched[j - 1])) {
            c->matched[j] = c->matched[j - 1];
            j--;
        }
        c->matched[j] = r;
    }
    /* Najpierw rozmiar czcionki (em w innych wlasnosciach od niego zalezy) */
    for (int pass = 0; pass < 2; pass++) {
        for (unsigned i = 0; i < count; i++)
            for (unsigned k = 0; k < c->matched[i]->ndecl; k++) {
                const struct decl *d = &c->matched[i]->decls[k];
                if (d->important || (d->prop == P_FONT_SIZE || d->prop == P_FONT) != (pass == 0))
                    continue;
                if (d->prop == P_CUSTOM) {
                    if (pass == 1 && n->tag == T_HTML && c->nvars < 256) {   /* zmienne strony */
                        c->var_name[c->nvars] = d->name;
                        c->var_value[c->nvars++] = d->value;
                    }
                    continue;
                }
                apply(c, st, parent, d);
            }
        if (pass == 0)
            presentational(c, st, n);
    }
    apply_inline(c, st, parent, n, 0);
    for (unsigned i = 0; i < count; i++)                 /* !important wygrywa ze wszystkim */
        for (unsigned k = 0; k < c->matched[i]->ndecl; k++)
            if (c->matched[i]->decls[k].important && c->matched[i]->decls[k].prop != P_CUSTOM)
                apply(c, st, parent, &c->matched[i]->decls[k]);
    apply_inline(c, st, parent, n, 1);
    if (n->tag == T_BODY && parent && !parent->has_bg && st->has_bg) {
        /* nic - tlo body rysuje uklad strony */
    }
    pick_font(st);

    if (st->display == D_NONE)
        return;                                            /* dzieci i tak nie beda widoczne */
    for (struct dom_node *ch = n->first; ch; ch = ch->next) {
        if (ch->type == N_TEXT)
            ch->style = st;                                /* tekst wyglada jak jego rodzic */
        else
            compute(c, mem, ch, st, counter);
    }
}

/* ---------- API ---------- */

struct css_ctx *css_new(int viewport_w)
{
    struct css_ctx *c = kzalloc(sizeof(*c));
    if (!c)
        return 0;
    c->viewport_w = viewport_w;
    c->origin = 0;
    parse_sheet(c, ua_sheet, ua_sheet + sizeof(ua_sheet) - 1);
    c->origin = 1;
    return c;
}

void css_add(struct css_ctx *c, const char *text, unsigned len)
{
    parse_sheet(c, text, text + len);
}

void css_compute(struct css_ctx *c, struct dom_doc *d)
{
    unsigned counter = 0;
    c->nvars = 0;
    struct style *root = d->root->style ? d->root->style : arena_alloc(&d->mem, sizeof(*root));
    if (!root)
        return;
    init_style(root, 0);
    root->display = D_BLOCK;
    root->color = 0x202122;
    pick_font(root);
    d->root->style = root;
    for (struct dom_node *ch = d->root->first; ch; ch = ch->next)
        if (ch->type == N_ELEMENT)
            compute(c, &d->mem, ch, root, &counter);
        else
            ch->style = root;
}

void css_free(struct css_ctx *c)
{
    if (!c)
        return;
    arena_free(&c->mem);
    kfree(c);
}

unsigned css_rule_count(const struct css_ctx *c)
{
    return c->nrules;
}

/* ---------- selektory dla JavaScriptu (querySelector) ---------- */

#define SEL_MAX 16

struct css_sel {
    int n;
    struct rule *rules[SEL_MAX];
};

struct css_sel *css_sel_parse(struct css_ctx *c, const char *text)
{
    struct css_sel *sel = arena_alloc(&c->mem, sizeof(*sel));
    if (!sel)
        return 0;
    const char *p = text, *start = text;
    int depth = 0;
    char q = 0;
    for (;; p++) {
        if (!*p || (*p == ',' && !depth && !q)) {
            struct rule *r = arena_alloc(&c->mem, sizeof(*r));
            if (!r || sel->n == SEL_MAX || !parse_selector(c, start, p, r))
                return 0;
            sel->rules[sel->n++] = r;
            if (!*p)
                break;
            start = p + 1;
        } else if (q) {
            if (*p == q)
                q = 0;
        } else if (*p == '"' || *p == '\'') {
            q = *p;
        } else if (*p == '(' || *p == '[') {
            depth++;
        } else if (*p == ')' || *p == ']') {
            depth--;
        }
    }
    return sel;
}

int css_sel_match(const struct css_sel *sel, const struct dom_node *n)
{
    for (int i = 0; i < sel->n; i++)
        if (match(sel->rules[i], sel->rules[i]->nparts - 1, n))
            return 1;
    return 0;
}

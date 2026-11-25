/* Uklad strony - patrz layout.h.
 *
 * Kazdy element blokowy (div, p, h1, li, table...) to prostokat:
 *     margines | ramka | wypelnienie (padding) | tresc
 * Bloki ukladamy jeden pod drugim. Tresc "w linii" (tekst, linki, obrazki,
 * przyciski) ukladamy w linie jak slowa w zeszycie - kazda linia ma linie
 * bazowa, na ktorej stoja litery i obrazki.
 *
 * Float (np. zdjecie po prawej stronie artykulu) wyjmujemy z biegu tekstu
 * i dosuwamy do krawedzi; linie obok niego sa krotsze. Floaty pamieta
 * "kontekst formatowania bloku" (BFC) - wspolny dla zagniezdzonych blokow.
 *
 * Elementy "atomowe" w linii (obrazek, inline-block, pole formularza)
 * najpierw ukladamy w punkcie (0,0), a potem przesuwamy na miejsce w linii.
 * Szerokosc "dopasowana do tresci" (floaty, tabele, inline-block) liczy
 * measure(): najwezsza (najdluzsze slowo) i najszersza (wszystko w jednej
 * linii) szerokosc elementu. */

#include "layout.h"
#include "svg.h"
#include "font.h"
#include "heap.h"
#include "string.h"
#include "thread.h"
#include "utf8.h"

#define FLOATS_MAX 64
#define COLS_MAX   64
#define ROWS_MAX   2000

struct flt {
    int x, y, w, h;
    int right;
};

struct bfc {
    struct flt f[FLOATS_MAX];
    int n;
};

/* Element z position: absolute/fixed - ukladany na koncu swojego "pudelka odniesienia"
 * (najblizszy przodek z position), bo dopiero wtedy znamy jego rozmiar */
struct absbox {
    struct dom_node *n;
    int sx, sy;                      /* gdzie stalby w zwyklym biegu strony */
    struct dom_node *cb;             /* pudelko odniesienia (NULL = cala strona) */
    int done;
};

/* Stan jednego ukladu */
struct lay {
    struct page_layout *L;
    const struct image *imgs;
    int nimgs;
    int link;                        /* biezacy link (wewnatrz <a href>) albo -1 */
    int li_number;                   /* numer punktu listy numerowanej */
    unsigned steps;
    struct dom_node *node;           /* element, ktory wlasnie ukladamy */
    struct absbox *abs;              /* elementy absolute/fixed czekajace na ulozenie */
    int nabs, abs_cap;
    struct dom_node *cb;             /* biezace pudelko odniesienia */
    struct dom_node *abs_self;       /* element absolute, ktorego szerokosc wlasnie mierzymy */
};

struct line {
    int active, has;
    unsigned start;                  /* pierwszy element linii */
    int top, x, left, right;
    int asc, desc;
    int pending_space;
};

/* Blok, ktorego zawartosc wlasnie ukladamy */
struct flow {
    struct lay *G;
    struct bfc *bfc;
    int left, right, y;
    int margin;                      /* dolny margines poprzedniego bloku (do "skladania") */
    const struct style *st;
    int *first_baseline;
    int blockify;                    /* flex w kolumnie: kazdy element jak blok */
    struct line ln;
};

static int layout_box(struct lay *G, struct dom_node *n, int x, int y, int avail, struct bfc *bfc,
                      int forced_w, int *out_w, int *baseline);
static void place_abs(struct lay *G, struct dom_node *cb, int start, int x, int y, int w, int h);
static void flow_children(struct flow *f, struct dom_node *parent);
static void measure(struct lay *G, struct dom_node *n, int *mn, int *mx);

static int max(int a, int b) { return a > b ? a : b; }
static int min(int a, int b) { return a < b ? a : b; }

/* ---------- lista rysowania ---------- */

static int add_item(struct lay *G, int kind)
{
    struct page_layout *L = G->L;
    if (L->n == L->cap) {
        unsigned cap = L->cap ? L->cap * 2 : 4096;
        struct dl_item *ni = kmalloc(sizeof(struct dl_item) * cap);
        if (!ni)
            return -1;
        if (L->items)
            memcpy(ni, L->items, sizeof(struct dl_item) * L->n);
        kfree(L->items);
        L->items = ni;
        L->cap = cap;
    }
    struct dl_item *it = &L->items[L->n];
    memset(it, 0, sizeof(*it));
    it->kind = (uint8_t)kind;
    it->link = -1;
    it->img = -1;
    it->node = G->node;
    return (int)L->n++;
}

/* border-radius w px dla pudelka w x h (procent liczony od krotszego boku, max polowa) */
static int has_radius(const struct style *st)
{
    for (int i = 0; i < 4; i++)
        if (st->radius[i].u != L_AUTO && st->radius[i].v > 0)
            return 1;
    return 0;
}

static void set_radii(int16_t rad[4], const struct style *st, int w, int h)
{
    int m = w < h ? w : h;
    for (int i = 0; i < 4; i++) {
        const struct css_len *l = &st->radius[i];
        int r = l->u == L_PCT ? (int)((int64_t)m * l->v / 10000) : l->u == L_PX ? l->v : 0;
        if (r < 0)
            r = 0;
        if (r > m / 2)
            r = m / 2;
        rad[i] = (int16_t)r;
    }
}

static void add_rect(struct lay *G, int x, int y, int w, int h, uint32_t color)
{
    if (w <= 0 || h <= 0)
        return;
    int i = add_item(G, DL_RECT);
    if (i < 0)
        return;
    struct dl_item *it = &G->L->items[i];
    it->x = x; it->y = y; it->w = w; it->h = h;
    it->color = color;
}

static int add_link(struct lay *G, const char *href)
{
    struct page_layout *L = G->L;
    if (L->nlinks == L->links_cap) {
        unsigned cap = L->links_cap ? L->links_cap * 2 : 256;
        const char **nl = kmalloc(sizeof(char *) * cap);
        if (!nl)
            return -1;
        if (L->links)
            memcpy(nl, L->links, sizeof(char *) * L->nlinks);
        kfree(L->links);
        L->links = nl;
        L->links_cap = cap;
    }
    L->links[L->nlinks] = href;
    return (int)L->nlinks++;
}

static void shift(struct lay *G, unsigned a, unsigned b, int dx, int dy)
{
    for (unsigned i = a; i < b; i++) {
        G->L->items[i].x += dx;
        G->L->items[i].y += dy;
    }
}

/* ---------- dlugosci ---------- */

static int resolve(struct css_len l, int base)
{
    if (l.u == L_PX)
        return l.v;
    if (l.u == L_PCT)
        return (int)((int64_t)base * l.v / 10000);
    return 0;
}

static int line_strut(const struct style *st)
{
    int fh = st->font == FONT_MONO ? 16 : font_height(st->font);
    int lh = st->line_px ? st->line_px : fh + 4;
    return max(lh, fh);
}

/* ---------- floaty ---------- */

/* Wolny pas [l, r) na wysokosci y..y+h (floaty zabieraja miejsce z bokow) */
static void edges(const struct flow *f, int y, int h, int *l, int *r)
{
    *l = f->left;
    *r = f->right;
    if (!f->bfc)
        return;
    for (int i = 0; i < f->bfc->n; i++) {
        const struct flt *fl = &f->bfc->f[i];
        if (fl->y < y + h && fl->y + fl->h > y) {
            if (!fl->right)
                *l = max(*l, fl->x + fl->w);
            else
                *r = min(*r, fl->x);
        }
    }
}

static int floats_bottom(const struct bfc *b, int side)
{
    int y = 0;
    for (int i = 0; b && i < b->n; i++)
        if (side == CL_BOTH || (side == CL_LEFT && !b->f[i].right) || (side == CL_RIGHT && b->f[i].right))
            y = max(y, b->f[i].y + b->f[i].h);
    return y;
}

/* ---------- linie ---------- */

static void line_begin(struct flow *f)
{
    struct line *ln = &f->ln;
    if (ln->active)
        return;
    ln->active = 1;
    ln->has = 0;
    ln->start = f->G->L->n;
    ln->top = f->y + f->margin;                  /* dolny margines bloku nad linia (np. <h4> i potem tekst) */
    edges(f, ln->top, line_strut(f->st), &ln->left, &ln->right);
    ln->x = ln->left;
    ln->asc = ln->desc = 0;
}

/* Zamyka linie; elementy od "end" (atom czekajacy na miejsce) juz do niej nie naleza */
static void finish_line_to(struct flow *f, unsigned end)
{
    struct line *ln = &f->ln;
    if (!ln->active)
        return;
    ln->active = 0;
    ln->pending_space = 0;
    if (!ln->has)
        return;
    int strut = line_strut(f->st);
    int content = ln->asc + ln->desc, h = max(content, strut);
    int baseline = ln->top + (h - content) / 2 + ln->asc;
    int dx = 0;
    if (f->st->align == AL_CENTER)
        dx = (ln->right - ln->x) / 2;
    else if (f->st->align == AL_RIGHT)
        dx = ln->right - ln->x;
    if (dx < 0)
        dx = 0;
    struct dl_item *items = f->G->L->items;
    for (unsigned i = ln->start; i < end; i++) {
        if (items[i].fixed)
            continue;                                /* float ma juz swoje miejsce */
        items[i].y = baseline - items[i].asc + items[i].rel;
        items[i].x += dx;
    }
    if (f->first_baseline && *f->first_baseline < 0)
        *f->first_baseline = baseline;
    f->y = ln->top + h;
    f->margin = 0;                               /* margines zuzyty przez linie */
}

static void finish_line(struct flow *f)
{
    finish_line_to(f, f->G->L->n);
}

/* Element "atomowy" (juz ulozony w (0,0), elementy a..b) - wstawiamy do linii.
 * Jego elementy juz sa na liscie, wiec linia musi je objac (start = a). */
static void place_atom(struct flow *f, unsigned a, unsigned b, int w, int h, int asc)
{
    struct line *ln = &f->ln;
    if (!ln->active) {
        line_begin(f);
        ln->start = a;
    }
    int sw = ln->pending_space && ln->has ? font_width(f->st->font == FONT_MONO ? FONT_REGULAR : f->st->font, " ", 1) : 0;
    if (ln->has && ln->x + sw + w > ln->right) {
        finish_line_to(f, a);                    /* atom przechodzi do nowej linii */
        line_begin(f);
        ln->start = a;
        sw = 0;
    }
    struct dl_item *items = f->G->L->items;
    for (unsigned i = a; i < b; i++) {
        items[i].x += ln->x + sw;
        items[i].rel = items[i].y;               /* wzgledem gory atomu */
        items[i].asc = (int16_t)asc;
        items[i].fixed = 0;
    }
    ln->x += sw + w;
    ln->asc = max(ln->asc, asc);
    ln->desc = max(ln->desc, h - asc);
    ln->has = 1;
    ln->pending_space = 0;
}

static void add_word(struct flow *f, const struct style *st, const char *text, unsigned len)
{
    struct line *ln = &f->ln;
    struct lay *G = f->G;
    int font = st->font;
    int w = font == FONT_MONO ? (int)len * 8 : font_width(font, text, len);
    int fh = font == FONT_MONO ? 16 : font_height(font), fa = font == FONT_MONO ? 12 : font_ascent(font);
    line_begin(f);
    int sw = ln->pending_space && ln->has ? font_width(font == FONT_MONO ? FONT_REGULAR : font, " ", 1) : 0;
    if (ln->has && ln->x + sw + w > ln->right && (st->white_space == WS_NORMAL || st->white_space == WS_PRE_LINE)) {
        finish_line(f);
        line_begin(f);
        sw = 0;
    }
    if (!st->hidden) {
        int i = add_item(G, DL_TEXT);
        if (i >= 0) {
            struct dl_item *it = &G->L->items[i];
            it->font = (uint8_t)font;
            it->x = ln->x + sw;
            it->w = w;
            it->h = fh;
            it->asc = (int16_t)fa;
            it->rel = 0;
            it->color = st->color;
            it->text = text;
            it->len = (uint16_t)(len > 65535 ? 65535 : len);
            it->link = G->link;
            it->underline = st->underline;
            /* Podkreslenie linku ciagle przez spacje miedzy slowami */
            if (sw && i > 0 && (unsigned)i - 1 >= ln->start) {
                const struct dl_item *prev = &G->L->items[i - 1];
                if (prev->kind == DL_TEXT && prev->link == it->link && prev->underline && it->underline)
                    it->ul_left = (int16_t)sw;
            }
        }
    }
    ln->x += sw + w;
    ln->asc = max(ln->asc, fa);
    ln->desc = max(ln->desc, fh - fa);
    ln->has = 1;
    ln->pending_space = 0;
}

static int is_ws(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f'; }

static void flow_text(struct flow *f, const struct dom_node *n)
{
    const struct style *st = n->style;
    const char *s = n->text, *end = n->text + n->text_len;
    if (st->white_space == WS_PRE) {                    /* <pre>: linie jak w pliku */
        struct style mono = *st;
        mono.font = FONT_MONO;
        while (s < end) {
            const char *e = s;
            while (e < end && *e != '\n')
                e++;
            if (e > s)
                add_word(f, &mono, s, (unsigned)(e - s));
            if (e < end) {                               /* koniec linii */
                line_begin(f);
                if (!f->ln.has)
                    f->y += 16;
                finish_line(f);
                e++;
            }
            s = e;
        }
        return;
    }
    while (s < end) {
        if (is_ws(*s)) {
            f->ln.pending_space = 1;
            while (s < end && is_ws(*s)) {
                /* white-space: pre-line - spacje zwijamy, ale koniec linii zostaje */
                if (*s == '\n' && st->white_space == WS_PRE_LINE) {
                    line_begin(f);
                    if (!f->ln.has)
                        f->y += line_strut(st);
                    finish_line(f);
                    f->ln.pending_space = 0;
                }
                s++;
            }
            continue;
        }
        const char *w = s;
        while (s < end && !is_ws(*s))
            s++;
        add_word(f, st, w, (unsigned)(s - w));
    }
}

/* ---------- obrazki i pola formularzy ---------- */

static int attr_px(const struct dom_node *n, const char *name)
{
    const char *v = dom_attr(n, name);
    if (!v)
        return -1;
    int x = 0, got = 0;
    for (; *v >= '0' && *v <= '9'; v++) {
        x = x * 10 + (*v - '0');
        got = 1;
    }
    return got && *v != '%' ? x : -1;
}

static int is_replaced(const struct dom_node *n)
{
    return n->tag == T_IMG || n->tag == T_CANVAS || n->tag == T_SVG;
}

/* Rozmiar obrazka (albo plotna <canvas>): z CSS, z atrybutow width/height albo prawdziwy */
static void image_size(struct lay *G, const struct dom_node *n, int avail, int *w, int *h)
{
    const struct style *st = n->style;
    const struct image *im = n->img >= 0 && n->img < G->nimgs && G->imgs[n->img].px ? &G->imgs[n->img] : 0;
    int canvas = n->tag == T_CANVAS;
    int aw = st->width.u != L_AUTO ? resolve(st->width, avail) : canvas ? -1 : attr_px(n, "width");
    int ah = st->height.u == L_PX ? st->height.v : canvas ? -1 : attr_px(n, "height");
    int iw = im ? im->w : 0, ih = im ? im->h : 0;
    if (canvas)                                          /* plotno: rozmiar z atrybutow (300 x 150) */
        dom_canvas_dims(n, &iw, &ih);
    if (n->tag == T_SVG)                                 /* svg: proporcje z viewBox */
        svg_size(n, &iw, &ih);
    if (aw > 0 && ah > 0) {
        *w = aw;
        *h = ah;
    } else if (aw > 0) {
        *w = aw;
        *h = iw ? (int)((int64_t)ih * aw / iw) : aw;
    } else if (ah > 0) {
        *h = ah;
        *w = ih ? (int)((int64_t)iw * ah / ih) : ah;
    } else {
        *w = iw;
        *h = ih;
    }
    if (st->max_width.u != L_AUTO) {                     /* max-width: 100% itp. */
        int mw = resolve(st->max_width, avail);
        if (*w > mw && *w > 0) {
            *h = (int)((int64_t)*h * mw / *w);
            *w = mw;
        }
    }
}

/* Obrazek albo pole formularza jako atom w linii. Zwraca 1, gdy cos ulozono */
static int replaced_atom(struct flow *f, struct dom_node *n)
{
    struct lay *G = f->G;
    const struct style *st = n->style;
    unsigned a = G->L->n;
    int w = 0, h = 0;
    if (n->tag == T_CANVAS || n->tag == T_SVG) {         /* svg rysujemy na wlasnym plotnie */
        image_size(G, n, f->right - f->left, &w, &h);
        if (w <= 0 || h <= 0)
            return 0;
        int i = add_item(G, DL_CANVAS);
        if (i >= 0) {
            struct dl_item *it = &G->L->items[i];
            it->w = w; it->h = h;
            it->link = G->link;
        }
        place_atom(f, a, G->L->n, w, h, h);              /* jak obrazek: dol na linii bazowej */
        return 1;
    }
    if (n->tag == T_IMG) {
        image_size(G, n, f->right - f->left, &w, &h);
        const struct image *im = n->img >= 0 && n->img < G->nimgs && G->imgs[n->img].px ? &G->imgs[n->img] : 0;
        if (im && w > 2 && h > 2) {
            int i = add_item(G, DL_IMAGE);
            if (i >= 0) {
                struct dl_item *it = &G->L->items[i];
                it->img = (int16_t)n->img;
                it->w = w; it->h = h;
                it->link = G->link;
                it->color = G->link >= 0 ? 0x3366CC : 0;
                if (n->style && has_radius(n->style))
                    set_radii(it->rad, n->style, w, h);
            }
        } else if (w > 2 && h > 2) {                    /* jeszcze sie laduje - miejsce na obrazek */
            unsigned first = G->L->n;
            add_rect(G, 0, 0, w, h, 0xEAECF0);
            if (G->L->n > first && n->style && has_radius(n->style))
                set_radii(G->L->items[first].rad, n->style, w, h);
        } else {
            const char *alt = dom_attr(n, "alt");
            if (!alt || !alt[0])
                return 0;
            w = font_width(FONT_REGULAR, alt, (unsigned)strlen(alt));
            h = font_height(FONT_REGULAR);
            int i = add_item(G, DL_TEXT);
            if (i >= 0) {
                struct dl_item *it = &G->L->items[i];
                it->font = FONT_REGULAR;
                it->w = w; it->h = h;
                it->y = 0;
                it->color = 0x72777D;
                it->text = alt;
                it->len = (uint16_t)strlen(alt);
                it->link = G->link;
            }
            place_atom(f, a, G->L->n, w, h, font_ascent(FONT_REGULAR));
            return 1;
        }
        place_atom(f, a, G->L->n, w, h + 2, h);
        return 1;
    }
    /* Pola formularza: prostokat z ramka i napisem */
    const char *label = 0;
    int field = 1;
    char buf[64];
    if (n->tag == T_INPUT) {
        const char *type = dom_attr(n, "type");
        if (type && (strstr_ci(type, "submit") || strstr_ci(type, "button") || strstr_ci(type, "reset"))) {
            label = dom_attr(n, "value");
            if (!label)
                label = strstr_ci(type, "reset") ? "Wyczy\xC5\x9B\xC4\x87" : "Wy\xC5\x9Blij";
            field = 0;
        } else if (type && (strstr_ci(type, "checkbox") || strstr_ci(type, "radio"))) {
            add_rect(G, 0, 0, 13, 13, 0x767676);
            add_rect(G, 1, 1, 11, 11, dom_attr(n, "checked") ? 0x3366CC : 0xFFFFFF);
            place_atom(f, a, G->L->n, 13, 13, 12);
            return 1;
        } else {
            label = dom_attr(n, "value");
            if (!label || !label[0])
                label = dom_attr(n, "placeholder");
        }
    } else if (n->tag == T_SELECT) {
        for (struct dom_node *o = n->first; o && !label; o = o->next)
            if (o->type == N_ELEMENT && o->tag == T_OPTION && o->first && o->first->type == N_TEXT) {
                unsigned l = o->first->text_len < sizeof(buf) - 1 ? o->first->text_len : sizeof(buf) - 1;
                memcpy(buf, o->first->text, l);
                buf[l] = '\0';
                label = buf;
            }
    } else if (n->tag == T_TEXTAREA) {
        label = dom_attr(n, "placeholder");
    }
    int fh = font_height(FONT_REGULAR);
    unsigned ll = label ? (unsigned)strlen(label) : 0;
    int tw = ll ? font_width(FONT_REGULAR, label, ll) : 0;
    if (!field) {
        w = tw + 20;
        h = fh + 8;
    } else if (n->tag == T_TEXTAREA) {
        w = 300;
        h = 64;
    } else {
        int size = attr_px(n, "size");
        w = size > 0 ? size * 8 + 12 : 200;
        if (n->tag == T_SELECT)
            w = tw + 34;
        h = fh + 8;
    }
    if (st->width.u != L_AUTO) {
        int cw = resolve(st->width, f->right - f->left);
        if (cw > 20)
            w = cw;
    }
    w = min(w, max(40, f->right - f->left));
    add_rect(G, 0, 0, w, h, 0x767676);                              /* ramka */
    add_rect(G, 1, 1, w - 2, h - 2, field ? 0xFFFFFF : 0xEFEFEF);
    if (ll) {
        int i = add_item(G, DL_TEXT);
        if (i >= 0) {
            struct dl_item *it = &G->L->items[i];
            it->font = FONT_REGULAR;
            it->x = field ? 6 : (w - tw) / 2;
            it->y = 4;
            it->w = min(tw, w - 8);
            it->h = fh;
            const char *val = dom_attr(n, "value");
            it->color = field && n->tag == T_INPUT && (!val || !val[0]) ? 0x72777D : 0x202122;
            it->text = label;
            it->len = (uint16_t)font_fit(FONT_REGULAR, label, ll, w - 12);
        }
    }
    if (n->tag == T_SELECT)                                          /* strzalka listy */
        add_rect(G, w - 16, h / 2 - 1, 8, 3, 0x202122);
    place_atom(f, a, G->L->n, w, h + 2, h - 5);
    return 1;
}

/* ---------- floaty i bloki w biegu tresci ---------- */

static int is_block_level(const struct style *st)
{
    return st->display == D_BLOCK || st->display == D_LIST_ITEM || st->display == D_TABLE ||
           st->display == D_ROW_GROUP || st->display == D_ROW || st->display == D_CELL ||
           st->display == D_CAPTION || st->display == D_FLEX;
}

static void place_float(struct flow *f, struct dom_node *n)
{
    struct lay *G = f->G;
    const struct style *st = n->style;
    int avail = f->right - f->left;
    int mt = resolve(st->margin[0], avail), mb = resolve(st->margin[2], avail);
    unsigned a = G->L->n;
    int mw, fb = -1;
    int h = layout_box(G, n, 0, 0, avail, 0, -1, &mw, &fb);
    unsigned b = G->L->n;
    int total_h = h + mt + mb;
    /* Szukamy miejsca: przy biezacej linii, a jesli sie nie miesci - nizej */
    int y = f->ln.active ? f->ln.top : f->y;
    for (int tries = 0; tries < FLOATS_MAX + 1; tries++) {
        int l, r;
        edges(f, y, total_h, &l, &r);
        if (r - l >= mw || !f->bfc)
            break;
        int next = 1 << 30;                               /* do konca najblizszego floata */
        for (int i = 0; i < f->bfc->n; i++) {
            const struct flt *fl = &f->bfc->f[i];
            if (fl->y < y + total_h && fl->y + fl->h > y)
                next = min(next, fl->y + fl->h);
        }
        if (next == 1 << 30)
            break;
        y = next;
    }
    int l, r;
    edges(f, y, total_h, &l, &r);
    int x = st->floating == FL_RIGHT ? r - mw : l;
    shift(G, a, b, x, y + mt);
    for (unsigned i = a; i < b; i++)
        G->L->items[i].fixed = 1;
    if (f->bfc && f->bfc->n < FLOATS_MAX) {
        struct flt *fl = &f->bfc->f[f->bfc->n++];
        fl->x = x;
        fl->y = y;
        fl->w = mw;
        fl->h = total_h;
        fl->right = st->floating == FL_RIGHT;
    }
    /* Biezaca linia obok nowego floata robi sie wezsza */
    if (f->ln.active && y == f->ln.top) {
        if (st->floating == FL_RIGHT) {
            f->ln.right = min(f->ln.right, x);
        } else if (x + mw > f->ln.left) {
            int dx = x + mw - f->ln.left;
            shift(G, f->ln.start, a, dx, 0);
            f->ln.left += dx;
            f->ln.x += dx;
        }
    }
}

static void block_child(struct flow *f, struct dom_node *n)
{
    struct lay *G = f->G;
    const struct style *st = n->style;
    int avail = f->right - f->left;
    finish_line(f);
    if (st->clear && f->bfc)
        f->y = max(f->y, floats_bottom(f->bfc, st->clear));
    int mt = resolve(st->margin[0], avail), mb = resolve(st->margin[2], avail);
    /* Marginesy pionowe sasiadow "skladaja sie" - liczy sie wiekszy */
    int gap = (f->margin >= 0 && mt >= 0) ? max(f->margin, mt) : f->margin + mt;
    int y = f->y + gap;
    int x = f->left;
    /* Blok z wlasnym BFC (np. overflow: hidden, tabela) omija floaty */
    if (f->bfc && (st->overflow || st->display == D_TABLE)) {
        int l, r;
        edges(f, y, 1, &l, &r);
        x = l;
        avail = r - l;
    }
    if (st->display == D_LIST_ITEM)
        G->li_number++;
    int mw, fb = -1;
    int saved = G->li_number;
    int h = layout_box(G, n, x, y, avail, f->bfc, -1, &mw, &fb);
    G->li_number = saved;
    if (f->first_baseline && *f->first_baseline < 0 && fb >= 0)
        *f->first_baseline = fb;
    f->y = y + h;
    f->margin = mb;
}

static void add_abs(struct lay *G, struct dom_node *n, int sx, int sy)
{

    if (G->nabs == G->abs_cap) {
        int cap = G->abs_cap ? G->abs_cap * 2 : 32;
        struct absbox *na = arena_alloc(&G->L->tmp, sizeof(struct absbox) * (unsigned)cap);
        if (!na)
            return;
        if (G->nabs)
            memcpy(na, G->abs, sizeof(struct absbox) * (unsigned)G->nabs);
        G->abs = na;
        G->abs_cap = cap;
    }
    struct absbox *a = &G->abs[G->nabs++];
    a->n = n;
    a->sx = sx;
    a->sy = sy;
    a->cb = n->style && n->style->position == POS_FIXED ? 0 : G->cb;
    a->done = 0;
}

static void flow_node(struct flow *f, struct dom_node *n)
{
    struct lay *G = f->G;
    if (++G->steps % 512 == 0)
        thread_yield();
    if (n->type == N_TEXT) {
        if (n->style)
            flow_text(f, n);
        return;
    }
    const struct style *st = n->style;
    if (!st || st->display == D_NONE)
        return;
    if (st->out_of_flow) {
        add_abs(G, n, f->ln.active ? f->ln.x : f->left, f->ln.active ? f->ln.top : f->y);
        return;
    }
    int saved_link = G->link;
    struct dom_node *saved_node = G->node;
    G->node = n;
    if (n->tag == T_A) {
        const char *href = dom_attr(n, "href");
        if (href)
            G->link = add_link(G, href);
    }
    if (st->floating != FL_NONE) {
        place_float(f, n);
    } else if (is_block_level(st) || f->blockify) {
        block_child(f, n);
    } else if (is_replaced(n) || n->tag == T_INPUT || n->tag == T_SELECT || n->tag == T_TEXTAREA) {
        replaced_atom(f, n);
    } else if (st->display == D_INLINE_BLOCK) {
        unsigned a = G->L->n;
        int mw, fb = -1;
        line_begin(f);
        int avail = f->ln.right - f->ln.left;
        int mt = resolve(st->margin[0], avail), mb = resolve(st->margin[2], avail);
        int h = layout_box(G, n, 0, mt, avail, 0, -1, &mw, &fb);
        place_atom(f, a, G->L->n, mw, h + mt + mb, fb >= 0 ? fb : h + mt);
    } else if (n->tag == T_BR) {
        line_begin(f);
        if (!f->ln.has)
            f->y += line_strut(st);                      /* pusta linia */
        finish_line(f);
    } else {
        flow_children(f, n);                             /* element w linii: <span>, <b>, <a>... */
    }
    G->link = saved_link;
    G->node = saved_node;
}

static void flow_children(struct flow *f, struct dom_node *parent)
{
    for (struct dom_node *c = parent->first; c; c = c->next)
        flow_node(f, c);
}

/* ---------- mierzenie ---------- */

static void measure_text(const struct dom_node *n, int *mn, int *mx)
{
    const struct style *st = n->style;
    int font = st->font, space = font_width(font, " ", 1);
    const char *s = n->text, *end = n->text + n->text_len;
    int line = 0;
    *mn = *mx = 0;
    if (st->white_space == WS_PRE) {
        while (s < end) {
            const char *e = s;
            while (e < end && *e != '\n')
                e++;
            *mx = max(*mx, (int)(e - s) * 8);
            s = e + 1;
        }
        *mn = *mx;
        return;
    }
    while (s < end) {
        if (is_ws(*s)) {
            while (s < end && is_ws(*s))
                s++;
            line += space;
            continue;
        }
        const char *w = s;
        while (s < end && !is_ws(*s))
            s++;
        int ww = font_width(font, w, (unsigned)(s - w));
        *mn = max(*mn, ww);
        line += ww;
    }
    *mx = line;
    if (st->white_space == WS_NOWRAP)                    /* bez zawijania: jedna linia */
        *mn = line;
}

static void measure_table(struct lay *G, struct dom_node *t, int *mn, int *mx);

static void measure(struct lay *G, struct dom_node *n, int *mn, int *mx)
{
    *mn = *mx = 0;
    if (n->type == N_TEXT) {
        if (n->style)
            measure_text(n, mn, mx);
        return;
    }
    const struct style *st = n->style;
    if (!st || st->display == D_NONE || (st->out_of_flow && n != G->abs_self))
        return;
    if (n->mgen == G->L->gen) {
        *mn = n->mmin;
        *mx = n->mmax;
        return;
    }
    int horiz = 0;
    for (int i = 1; i <= 3; i += 2)
        horiz += (st->padding[i].u == L_PX ? st->padding[i].v : 0) + st->border[i] +
                 (st->margin[i].u == L_PX ? st->margin[i].v : 0);
    if (is_replaced(n)) {
        int w, h;
        image_size(G, n, 100000, &w, &h);
        if (w <= 0) {
            const char *alt = dom_attr(n, "alt");
            w = alt ? font_width(FONT_REGULAR, alt, (unsigned)strlen(alt)) : 0;
        }
        *mn = *mx = w + horiz;
    } else if (n->tag == T_INPUT || n->tag == T_SELECT || n->tag == T_TEXTAREA) {
        *mn = *mx = 200;
    } else if (st->width.u == L_PX) {
        *mn = *mx = st->width.v + (st->border_box ? 0 : horiz) +
                    (st->border_box ? (st->margin[1].u == L_PX ? st->margin[1].v : 0) +
                                      (st->margin[3].u == L_PX ? st->margin[3].v : 0) : 0);
    } else if ((st->display == D_FLEX && !st->flex_column) || (st->grid && st->grid_n > 1)) {
        int count = 0;                                    /* rzad (albo siatka): szerokosci sie sumuja */
        for (struct dom_node *c = n->first; c; c = c->next) {
            int cmn, cmx;
            measure(G, c, &cmn, &cmx);                  /* tekst tez (anonimowy element) */
            if (!cmx && !cmn)
                continue;
            *mx += cmx + (count ? st->gap : 0);
            *mn = st->flex_wrap ? max(*mn, cmn) : *mn + cmn;
            count++;
        }
        *mn += horiz;
        *mx += horiz;
    } else if (st->display == D_TABLE) {
        measure_table(G, n, mn, mx);
        *mn += horiz;
        *mx += horiz;
    } else {
        int run = 0;                                      /* szerokosc biezacej "linii" */
        for (struct dom_node *c = n->first; c; c = c->next) {
            int cmn, cmx;
            measure(G, c, &cmn, &cmx);
            *mn = max(*mn, cmn);
            const struct style *cs = c->style;
            if (c->type == N_ELEMENT && cs && (is_block_level(cs) || c->tag == T_BR)) {
                *mx = max(*mx, run);
                *mx = max(*mx, cmx);
                run = 0;
            } else {
                run += cmx;
            }
        }
        *mx = max(*mx, run);
        *mn += horiz;
        *mx += horiz;
    }
    if (st->max_width.u == L_PX) {
        *mx = min(*mx, st->max_width.v + horiz);
        *mn = min(*mn, *mx);
    }
    n->mmin = *mn;
    n->mmax = *mx;
    n->mgen = G->L->gen;
}

/* ---------- tabele ---------- */

struct cell {
    struct dom_node *n;
    int col, span, row;
};

struct grid {
    struct cell *cells;
    int ncells, nrows, ncols;
    struct dom_node **rows;
};

static void collect_row(struct lay *G, struct grid *g, struct dom_node *tr)
{
    if (g->nrows >= ROWS_MAX)
        return;
    int r = g->nrows++;
    g->rows[r] = tr;
    int col = 0;
    for (struct dom_node *c = tr->first; c; c = c->next) {
        if (c->type != N_ELEMENT || !c->style || c->style->display == D_NONE)
            continue;
        int span = attr_px(c, "colspan");
        if (span < 1)
            span = 1;
        if (col + span > COLS_MAX)
            break;
        struct cell *ce = &g->cells[g->ncells++];
        ce->n = c;
        ce->col = col;
        ce->span = span;
        ce->row = r;
        col += span;
        if (g->ncells >= ROWS_MAX * 4)
            return;
    }
    g->ncols = max(g->ncols, col);
    (void)G;
}

static int build_grid(struct lay *G, struct dom_node *t, struct grid *g)
{
    memset(g, 0, sizeof(*g));
    g->rows = arena_alloc(&G->L->tmp, sizeof(struct dom_node *) * ROWS_MAX);
    g->cells = arena_alloc(&G->L->tmp, sizeof(struct cell) * ROWS_MAX * 4);
    if (!g->rows || !g->cells)
        return 0;
    for (struct dom_node *c = t->first; c; c = c->next) {
        if (c->type != N_ELEMENT || !c->style || c->style->display == D_NONE)
            continue;
        if (c->style->display == D_ROW_GROUP) {
            for (struct dom_node *r = c->first; r; r = r->next)
                if (r->type == N_ELEMENT && r->style && r->style->display != D_NONE)
                    collect_row(G, g, r);
        } else if (c->style->display != D_CAPTION) {
            collect_row(G, g, c);
        }
    }
    return 1;
}

static void column_widths(struct lay *G, struct grid *g, int *cmin, int *cmax)
{
    for (int i = 0; i < g->ncols; i++)
        cmin[i] = cmax[i] = 0;
    for (int pass = 0; pass < 2; pass++)                  /* najpierw komorki pojedyncze */
        for (int i = 0; i < g->ncells; i++) {
            struct cell *c = &g->cells[i];
            if ((c->span == 1) != (pass == 0))
                continue;
            int mn, mx;
            measure(G, c->n, &mn, &mx);
            int have_mn = 0, have_mx = 0;
            for (int k = 0; k < c->span; k++) {
                have_mn += cmin[c->col + k];
                have_mx += cmax[c->col + k];
            }
            for (int k = 0; k < c->span; k++) {            /* nadwyzke dzielimy po rowno */
                if (mn > have_mn)
                    cmin[c->col + k] += (mn - have_mn) / c->span;
                if (mx > have_mx)
                    cmax[c->col + k] += (mx - have_mx) / c->span;
            }
        }
    for (int i = 0; i < g->ncols; i++)
        cmax[i] = max(cmax[i], cmin[i]);
}

static void measure_table(struct lay *G, struct dom_node *t, int *mn, int *mx)
{
    struct grid g;
    int cmin[COLS_MAX], cmax[COLS_MAX];
    *mn = *mx = 0;
    if (!build_grid(G, t, &g))
        return;
    column_widths(G, &g, cmin, cmax);
    int sp = t->style->border_spacing;
    for (int i = 0; i < g.ncols; i++) {
        *mn += cmin[i];
        *mx += cmax[i];
    }
    *mn += sp * (g.ncols + 1);
    *mx += sp * (g.ncols + 1);
}

static int layout_table(struct lay *G, struct dom_node *t, int x, int y, int w, int fixed_width)
{
    int y0 = y;
    /* Podpis tabeli (caption) na gorze */
    for (struct dom_node *c = t->first; c; c = c->next)
        if (c->type == N_ELEMENT && c->style && c->style->display == D_CAPTION) {
            int mw, fb = -1;
            y += layout_box(G, c, x, y, w, 0, -1, &mw, &fb);
        }
    struct grid g;
    int cmin[COLS_MAX], cmax[COLS_MAX], cw[COLS_MAX];
    if (!build_grid(G, t, &g) || !g.ncols)
        return y - y0;
    column_widths(G, &g, cmin, cmax);
    int sp = t->style->border_spacing;
    int tmin = sp * (g.ncols + 1), tmax = tmin;
    for (int i = 0; i < g.ncols; i++) {
        tmin += cmin[i];
        tmax += cmax[i];
    }
    /* Szerokosci kolumn: od najwezszych do najszerszych, zeby zmiescic sie w w */
    int content_max = tmax - sp * (g.ncols + 1);
    for (int i = 0; i < g.ncols; i++) {
        if (w >= tmax) {
            cw[i] = cmax[i];
            if (fixed_width)                              /* tabela ma zadana szerokosc - rozciagamy */
                cw[i] += content_max ? (int)((int64_t)(w - tmax) * cmax[i] / content_max)
                                     : (w - tmax) / g.ncols;
        } else if (w <= tmin) {
            cw[i] = cmin[i];
        } else {
            cw[i] = cmin[i] + (int)((int64_t)(cmax[i] - cmin[i]) * (w - tmin) / max(1, tmax - tmin));
        }
    }
    int col_x[COLS_MAX + 1];
    col_x[0] = x + sp;
    for (int i = 0; i < g.ncols; i++)
        col_x[i + 1] = col_x[i] + cw[i] + sp;
    y += sp;
    int ci = 0;
    for (int r = 0; r < g.nrows; r++) {
        const struct style *rs = g.rows[r]->style;
        int row_bg = -1;
        if (rs && rs->has_bg) {
            row_bg = add_item(G, DL_RECT);
            if (row_bg >= 0) {
                G->L->items[row_bg].x = x;
                G->L->items[row_bg].y = y;
                G->L->items[row_bg].w = w;
                G->L->items[row_bg].color = rs->bg;
            }
        }
        int row_h = 0;
        unsigned first_item = G->L->n;
        int first_cell = ci;
        for (; ci < g.ncells && g.cells[ci].row == r; ci++) {
            struct cell *c = &g.cells[ci];
            int cx = col_x[c->col], cwid = col_x[c->col + c->span] - sp - cx;
            int mw, fb = -1;
            int h = layout_box(G, c->n, cx, y, cwid, 0, cwid, &mw, &fb);
            row_h = max(row_h, h);
        }
        /* Tla komorek do pelnej wysokosci wiersza */
        for (unsigned i = first_item; i < G->L->n; i++) {
            struct dl_item *it = &G->L->items[i];
            for (int k = first_cell; k < ci; k++)
                if (it->kind == DL_RECT && it->y == y && it->x == col_x[g.cells[k].col] &&
                    it->w == col_x[g.cells[k].col + g.cells[k].span] - sp - col_x[g.cells[k].col] && it->h > 1 &&
                    it->link == -2)
                    it->h = row_h;
        }
        if (row_bg >= 0)
            G->L->items[row_bg].h = row_h;
        y += row_h + sp;
    }
    return y - y0;
}

/* ---------- flexbox: elementy w rzedzie ----------
 * Kazdy element dostaje szerokosc "podstawowa" (flex-basis, width albo
 * szerokosc tresci). Gdy zostaje miejsce - rozdajemy je elementom z
 * flex-grow; gdy brakuje - sciskamy (nie wezej niz najdluzsze slowo) albo,
 * przy flex-wrap, przenosimy do nastepnego rzedu. */

#define FLEX_ITEMS 128

struct flex_item {
    struct dom_node *n;
    int base, mn, grow, shrink, ml, mr, mt, mb, w;
};

static int layout_flex_row(struct lay *G, struct dom_node *n, int x, int y, int w)
{
    const struct style *st = n->style;
    struct flex_item it[FLEX_ITEMS];
    int cnt = 0, gap = st->gap;
    for (struct dom_node *c = n->first; c && cnt < FLEX_ITEMS; c = c->next) {
        struct dom_node *e = c;                      /* element flexa (albo anonimowy) */
        if (c->type == N_TEXT) {
            /* Goly tekst w flexie - opakowujemy go w niewidzialny ("anonimowy") element */
            int text = 0;
            for (unsigned k = 0; k < c->text_len && !text; k++)
                text = !is_ws(c->text[k]);
            if (!text || !c->style)
                continue;
            struct dom_node *copy = arena_alloc(&G->L->tmp, sizeof(*copy));
            struct dom_node *anon = arena_alloc(&G->L->tmp, sizeof(*anon));
            struct style *as = arena_alloc(&G->L->tmp, sizeof(*as));
            if (!copy || !anon || !as)
                continue;
            *copy = *c;
            copy->next = copy->prev = 0;
            copy->parent = anon;
            *as = *c->style;                         /* wyglad tekstu po rodzicu, bez ramek i tla */
            as->display = D_BLOCK;
            as->floating = FL_NONE;
            as->has_bg = 0;
            as->width.u = as->height.u = as->max_width.u = as->min_width.u = as->flex_basis.u = L_AUTO;
            for (int k = 0; k < 4; k++) {
                as->margin[k].u = as->padding[k].u = L_PX;
                as->margin[k].v = as->padding[k].v = 0;
                as->border[k] = 0;
            }
            as->flex_grow = 0;
            anon->type = N_ELEMENT;
            anon->name = "#anon";
            anon->img = -1;
            anon->style = as;
            anon->first = anon->last = copy;
            anon->parent = n;
            copy->style = as;
            e = anon;
        }
        const struct style *cs = e->style;
        if (e->type == N_ELEMENT && cs && cs->display != D_NONE && cs->out_of_flow) {
            add_abs(G, e, x, y);
            continue;
        }
        if (e->type != N_ELEMENT || !cs || cs->display == D_NONE)
            continue;
        struct flex_item *f = &it[cnt++];
        int mx;
        f->n = e;
        measure(G, e, &f->mn, &mx);
        f->ml = cs->margin[3].u == L_AUTO ? 0 : resolve(cs->margin[3], w);
        f->mr = cs->margin[1].u == L_AUTO ? 0 : resolve(cs->margin[1], w);
        f->mt = resolve(cs->margin[0], w);
        f->mb = resolve(cs->margin[2], w);
        int horiz = resolve(cs->padding[1], w) + resolve(cs->padding[3], w) + cs->border[1] + cs->border[3];
        if (cs->flex_basis.u != L_AUTO)
            f->base = resolve(cs->flex_basis, w) + (cs->border_box ? 0 : horiz) + f->ml + f->mr;
        else if (cs->width.u != L_AUTO)
            f->base = resolve(cs->width, w) + (cs->border_box ? 0 : horiz) + f->ml + f->mr;
        else
            f->base = mx;
        if (cs->flex_basis.u == L_AUTO || cs->flex_grow == 0)
            f->base = max(f->base, 0);
        f->grow = cs->flex_grow;
        f->shrink = !cs->flex_shrink_none;
    }
    int cy = y, i = 0;
    while (i < cnt) {
        /* Jeden rzad: elementy od i do j-1 */
        int j = i, used = 0;
        while (j < cnt) {
            int add = it[j].base + (j > i ? gap : 0);
            if (st->flex_wrap && j > i && used + add > w)
                break;
            used += add;
            j++;
        }
        int free = w - used, grow_sum = 0, shrinkable = 0;
        for (int k = i; k < j; k++) {
            it[k].w = it[k].base;
            grow_sum += it[k].grow;
            if (it[k].shrink)
                shrinkable += max(0, it[k].base - it[k].mn);
        }
        if (free > 0 && grow_sum) {                       /* rozdajemy wolne miejsce */
            for (int k = i; k < j; k++)
                it[k].w += (int)((int64_t)free * it[k].grow / grow_sum);
        } else if (free < 0 && shrinkable) {               /* za ciasno - sciskamy */
            int need = min(-free, shrinkable);
            for (int k = i; k < j; k++)
                if (it[k].shrink)
                    it[k].w -= (int)((int64_t)need * max(0, it[k].base - it[k].mn) / shrinkable);
        }
        int line_used = gap * (j - i - 1);
        for (int k = i; k < j; k++)
            line_used += it[k].w;
        int rest = max(0, w - line_used), px = x, extra = 0;
        if (st->justify == JU_CENTER)
            px += rest / 2;
        else if (st->justify == JU_END)
            px += rest;
        else if (st->justify == JU_BETWEEN && j - i > 1)
            extra = rest / (j - i - 1);
        else if (st->justify == JU_AROUND) {
            extra = rest / (j - i);
            px += extra / 2;
        }
        int line_h = 0;
        for (int k = i; k < j; k++) {
            int bw = max(0, it[k].w - it[k].ml - it[k].mr), mw, fb = -1;
            int h = layout_box(G, it[k].n, px + it[k].ml, cy + it[k].mt, bw, 0, bw, &mw, &fb);
            line_h = max(line_h, h + it[k].mt + it[k].mb);
            px += it[k].w + gap + extra;
        }
        cy += line_h + (j < cnt ? gap : 0);
        i = j;
    }
    return cy - y;
}

/* ---------- CSS Grid (siatka) ----------
 * Kolumny z grid-template-columns: stale (px, %), "auto" (wedlug tresci)
 * i "fr" (czesc wolnego miejsca). Dzieci ukladamy po kolei: wiersz po
 * wierszu, od lewej; grid-column: span 2 zajmuje dwie kolumny, 1 / -1 caly
 * wiersz. Wysokosc wiersza = najwyzsze dziecko w nim. */

#define GRID_ITEMS 1024

struct gitem {
    struct dom_node *n;
    int col, span, row;
};

static int layout_grid(struct lay *G, struct dom_node *n, int x, int y, int w)
{
    const struct style *st = n->style;
    int gap = st->gap, rgap = st->row_gap;
    int ncol = st->grid_n ? st->grid_n : 1;
    uint8_t u[GRID_COLS];
    int v[GRID_COLS], mn[GRID_COLS], tw[GRID_COLS];
    for (int i = 0; i < ncol; i++) {
        u[i] = st->grid_n ? st->grid_u[i] : GT_FR;
        v[i] = st->grid_n ? st->grid_v[i] : 100;
        mn[i] = st->grid_n ? st->grid_min[i] : 0;
    }
    if (st->grid_fill) {                             /* repeat(auto-fill, minmax(200px, 1fr)) */
        int one = u[0] == GT_PX ? v[0] : u[0] == GT_PCT ? (int)((int64_t)w * v[0] / 10000) : mn[0];
        if (one <= 0)
            one = w;
        ncol = (w + gap) / (one + gap);
        if (ncol < 1)
            ncol = 1;
        if (ncol > GRID_COLS)
            ncol = GRID_COLS;
        for (int i = 1; i < ncol; i++) {
            u[i] = u[0];
            v[i] = v[0];
            mn[i] = mn[0];
        }
    }

    /* dzieci i ich miejsca w siatce */
    struct gitem *it = arena_alloc(&G->L->tmp, sizeof(struct gitem) * GRID_ITEMS);
    if (!it)
        return 0;
    int cnt = 0, row = 0, col = 0;
    for (struct dom_node *c = n->first; c && cnt < GRID_ITEMS; c = c->next) {
        const struct style *cs = c->style;
        if (c->type != N_ELEMENT || !cs || cs->display == D_NONE)
            continue;
        if (cs->out_of_flow) {
            add_abs(G, c, x, y);
            continue;
        }
        int span = cs->col_span ? cs->col_span : 1, start = 0;
        if (cs->col_start) {                         /* numery linii: 1 = lewa krawedz, -1 = prawa */
            start = cs->col_start > 0 ? cs->col_start : ncol + 2 + cs->col_start;
            if (cs->col_end) {
                int end = cs->col_end > 0 ? cs->col_end : ncol + 2 + cs->col_end;
                span = end - start;
            }
        }
        if (span < 1)
            span = 1;
        if (span > ncol)
            span = ncol;
        if (start >= 1) {
            int c0 = start - 1;
            if (c0 + span > ncol)
                c0 = ncol - span;
            if (c0 < col) {
                row++;
            }
            col = c0;
        } else if (col + span > ncol) {
            row++;
            col = 0;
        }
        it[cnt].n = c;
        it[cnt].col = col;
        it[cnt].span = span;
        it[cnt].row = row;
        cnt++;
        col += span;
    }

    /* szerokosci kolumn */
    int fixed = gap * (ncol - 1), fr_sum = 0, autos = 0;
    for (int i = 0; i < ncol; i++) {
        tw[i] = 0;
        if (u[i] == GT_PX)
            tw[i] = v[i];
        else if (u[i] == GT_PCT)
            tw[i] = (int)((int64_t)w * v[i] / 10000);
        else if (u[i] == GT_FR)
            fr_sum += v[i];
        else
            autos++;
    }
    for (int k = 0; k < cnt; k++) {                  /* "auto": najszersza tresc w kolumnie */
        if (it[k].span != 1 || u[it[k].col] != GT_AUTO)
            continue;
        int cmn, cmx;
        measure(G, it[k].n, &cmn, &cmx);
        tw[it[k].col] = max(tw[it[k].col], cmx);
    }
    for (int i = 0; i < ncol; i++)
        if (u[i] != GT_FR) {                         /* fr: minimum tylko jako dolna granica nizej */
            tw[i] = max(tw[i], mn[i]);
            fixed += tw[i];
        }
    int free = w - fixed;
    if (free > 0 && fr_sum) {
        for (int i = 0; i < ncol; i++)
            if (u[i] == GT_FR)
                tw[i] = max(mn[i], (int)((int64_t)free * v[i] / fr_sum));
    } else if (free > 0 && autos) {                  /* bez fr: kolumny auto sie rozciagaja */
        for (int i = 0; i < ncol; i++)
            if (u[i] == GT_AUTO)
                tw[i] += free / autos;
    } else if (free < 0 && autos) {                  /* za ciasno: sciskamy kolumny auto */
        int over = -free;
        for (int i = 0; i < ncol && over > 0; i++)
            if (u[i] == GT_AUTO) {
                int cut = min(over / autos + 1, tw[i] - mn[i]);
                if (cut > 0) {
                    tw[i] -= cut;
                    over -= cut;
                }
            }
    }
    int cx[GRID_COLS + 1];
    cx[0] = 0;
    for (int i = 0; i < ncol; i++)
        cx[i + 1] = cx[i] + tw[i] + gap;

    /* ukladanie wiersz po wierszu */
    int cy = y, k = 0;
    while (k < cnt) {
        int r = it[k].row, row_h = 0, first = k;
        unsigned items_at = G->L->n;
        int abs_at = G->nabs;
        for (int pass = 0; pass < 2; pass++) {
        if (pass) {
            /* Drugie podejscie: wszystkie w wierszu tak wysokie jak najwyzszy (align-items: stretch) */
            G->L->n = items_at;
            G->nabs = abs_at;
            k = first;
        }
        for (; k < cnt && it[k].row == r; k++) {
            struct style *cs = it[k].n->style;
            int iw = cx[it[k].col + it[k].span] - cx[it[k].col] - gap;
            int ml = cs->margin[3].u == L_AUTO ? 0 : resolve(cs->margin[3], iw);
            int mr = cs->margin[1].u == L_AUTO ? 0 : resolve(cs->margin[1], iw);
            int mt = resolve(cs->margin[0], iw), mb = resolve(cs->margin[2], iw);
            int bw = max(0, iw - ml - mr), mw, fb = -1, h;
            struct css_len saved_h = cs->height;
            if (pass && cs->height.u == L_AUTO && !is_replaced(it[k].n)) {
                int vert = resolve(cs->padding[0], iw) + resolve(cs->padding[2], iw) + cs->border[0] + cs->border[2];
                cs->height.u = L_PX;                 /* chwilowo stala wysokosc */
                cs->height.v = row_h - mt - mb - (cs->border_box ? 0 : vert);
            }
            if (cs->width.u != L_AUTO || is_replaced(it[k].n))   /* wlasna szerokosc - nie rozciagamy */
                h = layout_box(G, it[k].n, x + cx[it[k].col], cy + mt, iw, 0, -1, &mw, &fb);
            else
                h = layout_box(G, it[k].n, x + cx[it[k].col] + ml, cy + mt, bw, 0, bw, &mw, &fb);
            cs->height = saved_h;
            if (!pass)
                row_h = max(row_h, h + mt + mb);
        }
        int same = 1;                                /* jedno dziecko w wierszu - nie ma czego wyrownywac */
        for (int q = first + 1; q < k; q++)
            same = 0;
        if (same)
            break;
        }
        cy += row_h + (k < cnt ? rgap : 0);
    }
    return cy - y;
}

/* ---------- pudelko (element blokowy) ---------- */

static int layout_box(struct lay *G, struct dom_node *n, int x, int y, int avail, struct bfc *bfc,
                      int forced_w, int *out_w, int *baseline)
{
    const struct style *st = n->style;
    struct dom_node *saved_node = G->node;
    G->node = n;
    int m[4], p[4], b[4];
    for (int i = 0; i < 4; i++) {
        m[i] = st->margin[i].u == L_AUTO ? 0 : resolve(st->margin[i], avail);
        p[i] = resolve(st->padding[i], avail);
        b[i] = st->border[i];
    }
    int horiz = p[1] + p[3] + b[1] + b[3];
    /* Obrazek / plotno jako blok (display: block) ma swoja szerokosc, nie cala linie */
    int shrink = st->floating != FL_NONE || st->display == D_INLINE_BLOCK || st->out_of_flow ||
                 (st->display == D_TABLE && st->width.u == L_AUTO) || is_replaced(n);
    unsigned item_start = G->L->n;
    int abs_start = G->nabs;
    struct dom_node *saved_cb = G->cb;
    if (st->position != POS_STATIC)
        G->cb = n;
    int w;
    if (forced_w >= 0) {
        w = forced_w - horiz;
        m[1] = m[3] = 0;
    } else if (st->width.u != L_AUTO) {
        w = resolve(st->width, avail);
        if (st->border_box)
            w -= horiz;
    } else if (shrink) {
        /* "Dopasowana do tresci": najszersza tresc, ale nie szerzej niz jest miejsca
         * (i nie wezej niz najdluzsze slowo). measure() liczy razem z marginesami. */
        int mn, mx;
        measure(G, n, &mn, &mx);
        int fit = max(mn, min(mx, avail));
        w = fit - m[1] - m[3] - horiz;
    } else {
        w = avail - m[1] - m[3] - horiz;
    }
    if (st->max_width.u != L_AUTO) {
        int mw = resolve(st->max_width, avail) - (st->border_box ? horiz : 0);
        if (w > mw)
            w = mw;
    }
    if (st->min_width.u != L_AUTO) {
        int mw = resolve(st->min_width, avail) - (st->border_box ? horiz : 0);
        if (w < mw)
            w = mw;
    }
    if (w < 0)
        w = 0;
    /* margin: auto - wysrodkowanie (albo dosuniecie do prawej) */
    if (forced_w < 0 && st->floating == FL_NONE && st->display != D_INLINE_BLOCK) {
        int free = avail - w - horiz;
        if (st->margin[1].u == L_AUTO && st->margin[3].u == L_AUTO) {
            m[1] = m[3] = max(0, free / 2);
        } else if (st->margin[3].u == L_AUTO) {
            m[3] = max(0, free - m[1]);
        } else if (st->margin[1].u == L_AUTO) {
            m[1] = max(0, free - m[3]);
        }
    }
    int bx = x + m[3], bw = w + horiz;
    *out_w = bw + m[1] + m[3];

    int bg = -1, sh = -1;
    if (st->shadow.alpha && !st->hidden) {           /* cien pod spodem - przed tlem */
        sh = add_item(G, DL_SHADOW);
        if (sh >= 0) {
            struct dl_item *it = &G->L->items[sh];
            it->x = bx;
            it->y = y;
            it->w = bw;
            it->color = st->shadow.color;
            it->alpha = st->shadow.alpha;
            it->ext[0] = st->shadow.blur;
            it->ext[1] = st->shadow.spread;
            it->ext[2] = st->shadow.x;
            it->ext[3] = st->shadow.y;
        }
    }
    if ((st->has_bg || st->grad.type) && !st->hidden) {
        bg = add_item(G, st->grad.type ? DL_GRAD : DL_RECT);  /* gradient (rysuje tez kolor tla pod spodem) */
        if (bg >= 0) {
            struct dl_item *it = &G->L->items[bg];
            it->x = bx;
            it->y = y;
            it->w = bw;
            it->color = st->bg;
            it->link = -2;                               /* znak: tlo pudelka (tabela dopasuje wysokosc) */
        }
    }
    int cx = bx + b[3] + p[3], cy = y + b[0] + p[0];
    struct bfc *own = 0;
    if (!bfc || st->floating != FL_NONE || st->overflow || st->display == D_CELL ||
        st->display == D_INLINE_BLOCK || st->display == D_TABLE || st->display == D_CAPTION) {
        own = arena_alloc(&G->L->tmp, sizeof(*own));
        bfc = own;
    }
    int fb = -1, ch;
    if (st->display == D_TABLE) {
        ch = layout_table(G, n, cx, cy, w, st->width.u != L_AUTO);
    } else if (st->display == D_FLEX && !st->flex_column) {
        ch = layout_flex_row(G, n, cx, cy, w);
    } else if (st->grid && !is_replaced(n)) {
        ch = layout_grid(G, n, cx, cy, w);
    } else {
        struct flow f;
        memset(&f, 0, sizeof(f));
        f.G = G;
        f.bfc = bfc;
        f.left = cx;
        f.right = cx + w;
        f.y = cy;
        f.st = st;
        f.first_baseline = &fb;
        f.blockify = st->display == D_FLEX;
        int li = G->li_number;                       /* zagniezdzona lista liczy od nowa */
        G->li_number = 0;
        if (is_replaced(n)) {                        /* blok z obrazkiem/plotnem: sama zawartosc */
            replaced_atom(&f, n);
        } else {
            flow_children(&f, n);
        }
        finish_line(&f);
        G->li_number = li;
        ch = f.y - cy + (f.margin > 0 ? f.margin : 0);
        if (own)                                         /* wlasny BFC obejmuje swoje floaty */
            ch = max(ch, floats_bottom(own, CL_BOTH) - cy);
    }
    if (st->height.u == L_PX)
        ch = st->height.v - (st->border_box ? p[0] + p[2] + b[0] + b[2] : 0);
    if (ch < 0)
        ch = 0;
    int h = ch + p[0] + p[2] + b[0] + b[2];
    int rounded = has_radius(st);
    if (bg >= 0) {
        G->L->items[bg].h = h;
        if (rounded)
            set_radii(G->L->items[bg].rad, st, bw, h);
    }
    if (sh >= 0) {
        struct dl_item *it = &G->L->items[sh];
        it->h = h;
        if (rounded)
            set_radii(it->rad, st, bw, h);
    }
    /* Ramki */
    if (!st->hidden && rounded && (b[0] || b[1] || b[2] || b[3])) {
        int i = add_item(G, DL_RING);                /* zaokraglona ramka - jednym kawalkiem */
        if (i >= 0) {
            struct dl_item *it = &G->L->items[i];
            it->x = bx;
            it->y = y;
            it->w = bw;
            it->h = h;
            it->color = st->border_color[b[0] ? 0 : b[1] ? 1 : b[2] ? 2 : 3];
            set_radii(it->rad, st, bw, h);
            for (int k = 0; k < 4; k++)
                it->ext[k] = (int16_t)b[k];
        }
    } else if (!st->hidden) {
        if (b[0]) add_rect(G, bx, y, bw, b[0], st->border_color[0]);
        if (b[2]) add_rect(G, bx, y + h - b[2], bw, b[2], st->border_color[2]);
        if (b[3]) add_rect(G, bx, y, b[3], h, st->border_color[3]);
        if (b[1]) add_rect(G, bx + bw - b[1], y, b[1], h, st->border_color[1]);
    }
    /* Znacznik punktu listy: kropka albo numer, na wysokosci pierwszej linii */
    if (st->display == D_LIST_ITEM && st->list != LS_NONE && !st->hidden) {
        int base = fb >= 0 ? fb : cy + font_ascent(st->font);
        if (st->list == LS_DECIMAL) {
            char *num = arena_alloc(&G->L->tmp, 12);
            if (num) {
                ksnprintf(num, 12, "%d.", G->li_number);
                unsigned nl = (unsigned)strlen(num);
                int tw = font_width(st->font, num, nl);
                int i = add_item(G, DL_TEXT);
                if (i >= 0) {
                    struct dl_item *it = &G->L->items[i];
                    it->font = st->font;
                    it->x = cx - tw - 6;
                    it->y = base - font_ascent(st->font);
                    it->w = tw;
                    it->h = font_height(st->font);
                    it->color = st->color;
                    it->text = num;
                    it->len = (uint16_t)nl;
                }
            }
        } else {
            int s = 5, mx0 = cx - 13, my0 = base - font_ascent(st->font) / 2 - 3;
            if (st->list == LS_CIRCLE) {
                add_rect(G, mx0, my0, s, s, st->color);
                add_rect(G, mx0 + 1, my0 + 1, s - 2, s - 2, 0xFFFFFF);
            } else {
                add_rect(G, mx0, my0, s, s, st->color);
            }
        }
    }
    if (baseline)
        *baseline = fb;
    if (st->position != POS_STATIC) {
        G->cb = saved_cb;
        /* elementy absolute wzgledem tego pudelka (jego wnetrza z paddingiem) */
        place_abs(G, n, abs_start, bx + b[3], y + b[0], bw - b[1] - b[3], h - b[0] - b[2]);
        if (st->position == POS_RELATIVE) {
            int dx = 0, dy = 0;
            if (st->inset[3].u != L_AUTO) dx = resolve(st->inset[3], avail);
            else if (st->inset[1].u != L_AUTO) dx = -resolve(st->inset[1], avail);
            if (st->inset[0].u != L_AUTO) dy = resolve(st->inset[0], h);
            else if (st->inset[2].u != L_AUTO) dy = -resolve(st->inset[2], h);
            if (dx || dy)
                shift(G, item_start, G->L->n, dx, dy);
        }
    }
    if (st->tx.u != L_AUTO || st->ty.u != L_AUTO) {       /* transform: translate(...) */
        int dx = st->tx.u == L_AUTO ? 0 : resolve(st->tx, bw);
        int dy = st->ty.u == L_AUTO ? 0 : resolve(st->ty, h);
        if (dx || dy)
            shift(G, item_start, G->L->n, dx, dy);
    }
    G->node = saved_node;
    return h;
}

/* Element position: absolute w pudelku odniesienia (x, y, w, h) */
static void layout_abs(struct lay *G, struct absbox *a, int cbx, int cby, int cbw, int cbh)
{
    struct dom_node *n = a->n;
    const struct style *st = n->style;
    if (!st || st->display == D_NONE)
        return;
    /* "ukryte dla oka, widoczne dla czytnikow ekranu" (width: 1px; height: 1px) - pomijamy */
    if ((st->width.u == L_PX && st->width.v <= 1) || (st->height.u == L_PX && st->height.v <= 1))
        return;
    int has_l = st->inset[3].u != L_AUTO, has_r = st->inset[1].u != L_AUTO;
    int has_t = st->inset[0].u != L_AUTO, has_b = st->inset[2].u != L_AUTO;
    int left = has_l ? resolve(st->inset[3], cbw) : 0, right = has_r ? resolve(st->inset[1], cbw) : 0;
    int top = has_t ? resolve(st->inset[0], cbh) : 0, bottom = has_b ? resolve(st->inset[2], cbh) : 0;
    int forced = -1;
    if (st->width.u == L_AUTO && has_l && has_r && cbw - left - right > 0)
        forced = cbw - left - right;
    unsigned first = G->L->n;
    int mw, fb = -1;
    struct dom_node *saved_node = G->node;
    int saved_link = G->link;
    G->link = -1;
    struct dom_node *saved_self = G->abs_self;
    G->abs_self = n;
    int h = layout_box(G, n, 0, 0, cbw > 0 ? cbw : 1, 0, forced, &mw, &fb);
    G->abs_self = saved_self;
    G->link = saved_link;
    G->node = saved_node;
    int x = has_l ? cbx + left : has_r ? cbx + cbw - right - mw : a->sx;
    int y = has_t ? cby + top : has_b ? cby + cbh - bottom - h : a->sy;
    shift(G, first, G->L->n, x, y);
    for (unsigned i = first; i < G->L->n; i++)
        G->L->items[i].fixed = 1;
}

static void place_abs(struct lay *G, struct dom_node *cb, int start, int x, int y, int w, int h)
{
    for (int i = start; i < G->nabs; i++) {
        if (G->abs[i].done || (cb && G->abs[i].cb != cb))
            continue;
        G->abs[i].done = 1;
        struct absbox a = G->abs[i];                     /* tablica moze urosnac w trakcie */
        layout_abs(G, &a, x, y, w, h);
    }
}

/* ---------- strona ---------- */

static struct dom_node *find_tag(struct dom_node *n, int tag, int depth)
{
    for (struct dom_node *c = n->first; c && depth < 3; c = c->next) {
        if (c->type == N_ELEMENT && c->tag == tag)
            return c;
        if (c->type == N_ELEMENT) {
            struct dom_node *r = find_tag(c, tag, depth + 1);
            if (r)
                return r;
        }
    }
    return 0;
}

void layout_page(struct page_layout *L, struct dom_doc *d, int width, const struct image *imgs, int nimgs)
{
    L->n = 0;
    L->nlinks = 0;
    L->width = width;
    L->gen++;
    arena_free(&L->tmp);
    struct lay G;
    memset(&G, 0, sizeof(G));
    G.L = L;
    G.imgs = imgs;
    G.nimgs = nimgs;
    G.link = -1;
    /* Tlo strony: z <body>, a jesli nie ma - z <html> */
    L->bg = 0xFFFFFF;
    struct dom_node *html = find_tag(d->root, T_HTML, 0), *body = find_tag(d->root, T_BODY, 0);
    if (html && html->style && html->style->has_bg)
        L->bg = html->style->bg;
    if (body && body->style && body->style->has_bg)
        L->bg = body->style->bg;

    struct bfc *root = arena_alloc(&L->tmp, sizeof(*root));
    struct flow f;
    memset(&f, 0, sizeof(f));
    f.G = &G;
    f.bfc = root;
    f.left = 0;
    f.right = width;
    f.y = 0;
    f.st = d->root->style;
    if (!f.st)
        return;
    flow_children(&f, d->root);
    finish_line(&f);
    L->height = max(f.y + max(f.margin, 0), floats_bottom(root, CL_BOTH)) + 8;
    /* position: fixed i absolute bez przodka z position - wzgledem calej strony */
    place_abs(&G, 0, 0, 0, 0, width, L->height);
}

void layout_free(struct page_layout *L)
{
    kfree(L->items);
    kfree(L->links);
    arena_free(&L->tmp);
    memset(L, 0, sizeof(*L));
}

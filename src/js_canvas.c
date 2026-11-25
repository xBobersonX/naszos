/* <canvas> - rysowanie z JavaScriptu (canvas.getContext("2d")).
 *
 * Plotno to tablica pikseli 0xAARRGGBB (dom.h: struct dom_canvas). Skrypt
 * rysuje po niej, a przegladarka tylko ja pokazuje na stronie.
 *
 * Jak rysujemy ksztalty: kazda sciezka (prostokat, kolo, linia...) to wielokat
 * - krzywe i luki zamieniamy na wiele krotkich odcinkow. Wielokat wypelniamy
 * wiersz po wierszu ("scanline"): w kazdym wierszu pikseli sprawdzamy 4 linie
 * posrednie, gdzie przecinaja je krawedzie, i liczymy, jaka czesc kazdego
 * piksela lezy w srodku. Ta czesc (0..1) to "pokrycie" - dzieki niemu krawedzie
 * sa wygladzone, a nie schodkowe. Grubą linie (stroke) zamieniamy na wielokaty
 * (prostokat na kazdy odcinek + polaczenia i zakonczenia) i wypelniamy tak samo.
 *
 * Przeksztalcenia (translate, rotate, scale) to macierz 2x3: punkty sciezki
 * przeliczamy na piksele plotna juz przy dodawaniu (jak w specyfikacji).
 *
 * Kolory trzymamy bez mnozenia przez alfe; laczenie z tlem (source-over itp.)
 * liczymy na liczbach zmiennoprzecinkowych - ten plik, jak caly silnik JS,
 * jest kompilowany z SSE. */

#include "js_int.h"
#include "string.h"
#include "heap.h"
#include "dom.h"
#include "css.h"
#include "font.h"

#define U __attribute__((unused))
#define NATIVE(name) static jv name(js_rt *rt U, jv self U, int argc U, jv *argv U, jobj *fn U)
#define ARG(i) ((i) < argc ? argv[i] : jv_undef())

#define PI 3.14159265358979323846
#define SUBROWS 4                        /* linie posrednie w jednym wierszu pikseli */
#define STACK_MAX 64                     /* glebokosc save() */

enum { CK_CTX = 1, CK_GRAD };
enum { PT_COLOR, PT_LINEAR, PT_RADIAL };
enum { CO_OVER, CO_DEST_OVER, CO_DEST_OUT, CO_LIGHTER, CO_COPY, CO_SRC_ATOP, CO_DEST_IN, CO_SRC_IN, CO_XOR, CO_CLEAR };
enum { LC_BUTT, LC_ROUND, LC_SQUARE };
enum { LJ_MITER, LJ_ROUND, LJ_BEVEL };
enum { TA_START, TA_LEFT, TA_CENTER, TA_RIGHT, TA_END };
enum { TB_ALPHABETIC, TB_TOP, TB_HANGING, TB_MIDDLE, TB_IDEOGRAPHIC, TB_BOTTOM };

struct gstop { double t, r, g, b, a; };

struct cgrad {                           /* createLinearGradient / createRadialGradient */
    int kind;                            /* CK_GRAD */
    int type;                            /* PT_LINEAR / PT_RADIAL */
    double x0, y0, r0, x1, y1, r1;
    struct gstop *stops;
    int n, cap;
};

struct paint {
    int type;                            /* PT_COLOR albo rodzaj gradientu */
    double r, g, b, a;                   /* kolor 0..255, alfa 0..1 */
    struct cgrad *grad;
    jv value;                            /* to, co zwraca getter (gradient) */
};

struct cstate {
    double m[6];                         /* x' = m0*x + m2*y + m4, y' = m1*x + m3*y + m5 */
    struct paint fill, stroke;
    double alpha, line_width, miter;
    int cap, join, op, smooth;
    int face;                            /* czcionka NaszOS (font.h) */
    double font_px;
    jstr *font;
    int align, baseline;
    int clip;                            /* -1 = bez obcinania, inaczej numer maski */
};

struct cpt { double x, y; };
struct csub { int start, n, closed; };

struct cedge {
    double x0, y0, x1, y1;               /* y0 < y1 */
    double dxdy;
    int dir;
};

struct c2d {
    int kind;                            /* CK_CTX */
    struct dom_node *node;
    jobj *self;
    int w, h;                            /* rozmiar plotna, dla ktorego jest stan */
    struct cstate st;
    struct cstate stack[STACK_MAX];
    int sp;
    /* biezaca sciezka (w pikselach plotna) */
    struct cpt *pts;
    int npts, pcap;
    struct csub *subs;
    int nsubs, scap;
    /* robocze */
    struct cedge *e;
    int ne, ecap;
    int *order;
    int ocap;
    struct cpt *tmp;                     /* stroke: punkty bez powtorzen */
    int tcap;
    float *cov;                          /* pokrycie jednego wiersza (w + 2) */
    uint8_t *masks[STACK_MAX + 1];       /* maski obcinania (clip) - po jednej na poziom save() */
    int mask_w, mask_h;
    double lut[256][4];                  /* kolory gradientu */
};

/* ---------- drobiazgi ---------- */

static inline int ifloor(double x) { int i = (int)x; return i - (x < i); }
static inline double dmin(double a, double b) { return a < b ? a : b; }
static inline double dmax(double a, double b) { return a > b ? a : b; }
static inline double clamp01(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

static jv str8(js_rt *rt, const char *s) { return jv_str(js_str_utf8(rt, s, (uint32_t)strlen(s))); }

static int lower_eq(const char *s, const char *t)
{
    for (; *s && *t; s++, t++) {
        char a = *s >= 'A' && *s <= 'Z' ? (char)(*s + 32) : *s;
        if (a != *t)
            return 0;
    }
    return !*s && !*t;
}

/* Argumenty liczbowe: 0 = ktorys nie jest skonczona liczba (wtedy spec kaze nic nie robic) */
static int nums(js_rt *rt, int argc, jv *argv, int n, double *out)
{
    int ok = 1;
    for (int i = 0; i < n; i++) {
        out[i] = js_to_num(rt, ARG(i));
        if (!js_isfinite(out[i]))
            ok = 0;
    }
    return ok;
}

static void grow(js_rt *rt, void **arr, int *cap, int need, size_t size)
{
    if (need <= *cap)
        return;
    int nc = *cap ? *cap * 2 : 64;
    while (nc < need)
        nc *= 2;
    void *na = rt ? js_alloc(rt, size * (size_t)nc) : kmalloc(size * (size_t)nc);   /* rt = 0: rysowanie SVG z C */
    if (!na)
        return;
    if (*cap)
        memcpy(na, *arr, size * (size_t)*cap);
    if (!rt)
        kfree(*arr);
    *arr = na;
    *cap = nc;
}

/* ---------- kolory CSS ---------- */

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    return s;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Liczby w nawiasie: rgb(1, 2, 3 / 0.5), hsl(120deg 50% 50%). pct[i] = mialo "%" */
static int func_args(const char *s, double *v, int *pct, int max)
{
    int n = 0;
    s = skip_ws(s);
    if (*s != '(')
        return -1;
    s++;
    while (n < max) {
        s = skip_ws(s);
        if (*s == ',' || *s == '/') {
            s++;
            continue;
        }
        if (*s == ')')
            break;
        const char *end;
        double d = js_strtod(s, &end);
        if (end == s)
            return -1;
        s = end;
        pct[n] = 0;
        if (*s == '%') {
            pct[n] = 1;
            s++;
        } else {
            while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z'))
                s++;                                 /* deg, turn... - traktujemy jak stopnie */
        }
        v[n++] = d;
    }
    s = skip_ws(s);
    return *s == ')' ? n : -1;
}

static double hue(double p, double q, double t)
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

/* Kolor CSS -> r, g, b (0..255), a (0..1). 0 = nie rozumiemy (spec: zostaje stary) */
static int parse_color(const char *s, double *rgba)
{
    s = skip_ws(s);
    if (*s == '#') {
        int d[8], n = 0;
        for (s++; hexv(*s) >= 0 && n < 8; s++)
            d[n++] = hexv(*s);
        if (*skip_ws(s))
            return 0;
        if (n == 3 || n == 4) {
            rgba[0] = d[0] * 17; rgba[1] = d[1] * 17; rgba[2] = d[2] * 17;
            rgba[3] = n == 4 ? d[3] * 17 / 255.0 : 1;
            return 1;
        }
        if (n == 6 || n == 8) {
            rgba[0] = d[0] * 16 + d[1]; rgba[1] = d[2] * 16 + d[3]; rgba[2] = d[4] * 16 + d[5];
            rgba[3] = n == 8 ? (d[6] * 16 + d[7]) / 255.0 : 1;
            return 1;
        }
        return 0;
    }
    char word[8];
    int wl = 0;
    const char *p = s;
    while (((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) && wl < 7)
        word[wl++] = (char)(*p >= 'A' && *p <= 'Z' ? *p++ + 32 : *p++);
    word[wl] = 0;
    double v[4];
    int pct[4];
    if (strcmp(word, "rgb") == 0 || strcmp(word, "rgba") == 0) {
        int n = func_args(p, v, pct, 4);
        if (n < 3)
            return 0;
        for (int i = 0; i < 3; i++)
            rgba[i] = dmax(0, dmin(255, pct[i] ? v[i] * 2.55 : v[i]));
        rgba[3] = n > 3 ? clamp01(pct[3] ? v[3] / 100 : v[3]) : 1;
        return 1;
    }
    if (strcmp(word, "hsl") == 0 || strcmp(word, "hsla") == 0) {
        int n = func_args(p, v, pct, 4);
        if (n < 3)
            return 0;
        double h = js_fmod(v[0], 360) / 360, sat = clamp01(v[1] / 100), l = clamp01(v[2] / 100);
        if (h < 0)
            h += 1;
        double q = l < 0.5 ? l * (1 + sat) : l + sat - l * sat, pp = 2 * l - q;
        rgba[0] = 255 * hue(pp, q, h + 1.0 / 3);
        rgba[1] = 255 * hue(pp, q, h);
        rgba[2] = 255 * hue(pp, q, h - 1.0 / 3);
        rgba[3] = n > 3 ? clamp01(pct[3] ? v[3] / 100 : v[3]) : 1;
        return 1;
    }
    if (lower_eq(s, "transparent")) {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
        return 1;
    }
    if (lower_eq(s, "currentcolor")) {
        rgba[0] = rgba[1] = rgba[2] = 0;
        rgba[3] = 1;
        return 1;
    }
    uint32_t c;
    if (!css_named_color(s, &c))
        return 0;
    rgba[0] = c >> 16 & 255; rgba[1] = c >> 8 & 255; rgba[2] = c & 255; rgba[3] = 1;
    return 1;
}

/* Kolor jak zwraca przegladarka: "#ff8800" albo "rgba(255, 136, 0, 0.5)" */
static jv color_value(js_rt *rt, const struct paint *p)
{
    if (p->type != PT_COLOR)
        return p->value;
    int r = (int)(p->r + 0.5), g = (int)(p->g + 0.5), b = (int)(p->b + 0.5);
    char buf[64];
    if (p->a >= 1) {
        static const char hex[] = "0123456789abcdef";
        buf[0] = '#';
        int c[3] = { r, g, b };
        for (int i = 0; i < 3; i++) {
            buf[1 + i * 2] = hex[c[i] >> 4];
            buf[2 + i * 2] = hex[c[i] & 15];
        }
        buf[7] = 0;
    } else {
        char a[32];
        js_dtoa(js_round(p->a * 1000) / 1000, a);
        ksnprintf(buf, sizeof(buf), "rgba(%d, %d, %d, %s)", r, g, b, a);
    }
    return str8(rt, buf);
}

/* ---------- kontekst ---------- */

static void reset_state(struct cstate *s)
{
    memset(s, 0, sizeof(*s));
    s->m[0] = s->m[3] = 1;
    s->fill.a = s->stroke.a = 1;                 /* czarny */
    s->fill.value = s->stroke.value = jv_undef();
    s->alpha = 1;
    s->line_width = 1;
    s->miter = 10;
    s->smooth = 1;
    s->face = FONT_SMALL;
    s->font_px = 10;
    s->clip = -1;
}

/* Obiekt JC_CANVAS2D: kontekst albo gradient (oba zaczynaja sie od pola kind) */
static int kind_of(const jobj *o)
{
    return o->cls == JC_CANVAS2D && o->u.cv.c ? *(const int *)o->u.cv.c : 0;
}

static struct c2d *CTX(js_rt *rt, jv self)
{
    if (self.t != JT_OBJ || kind_of(self.u.o) != CK_CTX)
        js_throw_error(rt, JS_ERR_TYPE, "Illegal invocation");
    return self.u.o->u.cv.c;
}

/* Plotno ma nowy rozmiar: stan kontekstu od poczatku (jak w spec) */
static void ctx_resized(struct c2d *c, const struct dom_canvas *cv, int reset)
{
    c->w = cv->w;
    c->h = cv->h;
    c->cov = 0;                                      /* bufor wiersza - od nowa, na nowa szerokosc */
    if (reset) {
        reset_state(&c->st);
        c->sp = 0;
        c->npts = c->nsubs = 0;
    }
}

/* Piksele plotna (po zmianie atrybutow width/height - nowe, puste) */
static struct dom_canvas *pixels(js_rt *rt, struct c2d *c)
{
    if (!rt->doc)
        return 0;
    struct dom_canvas *cv = dom_canvas_sync(rt->doc, c->node);
    if (!cv)
        return 0;
    if (cv->w != c->w || cv->h != c->h)
        ctx_resized(c, cv, 1);
    if (!cv->px)
        return 0;
    if (!c->cov)
        c->cov = js_alloc_raw(rt, sizeof(float) * (size_t)(cv->w + 2));
    return cv;
}

static void changed(js_rt *rt)
{
    if (rt && rt->doc)
        rt->doc->canvas_gen++;
}

/* ---------- macierz ---------- */

static void apply(const double *m, double x, double y, double *ox, double *oy)
{
    *ox = m[0] * x + m[2] * y + m[4];
    *oy = m[1] * x + m[3] * y + m[5];
}

static int invert(const double *m, double *inv)
{
    double det = m[0] * m[3] - m[1] * m[2];
    if (det == 0 || !js_isfinite(det))
        return 0;
    inv[0] = m[3] / det;
    inv[1] = -m[1] / det;
    inv[2] = -m[2] / det;
    inv[3] = m[0] / det;
    inv[4] = -(inv[0] * m[4] + inv[2] * m[5]);
    inv[5] = -(inv[1] * m[4] + inv[3] * m[5]);
    return 1;
}

static void mul(double *m, const double *t)       /* m = m * t (t dziala pierwsze) */
{
    double r[6];
    r[0] = m[0] * t[0] + m[2] * t[1];
    r[1] = m[1] * t[0] + m[3] * t[1];
    r[2] = m[0] * t[2] + m[2] * t[3];
    r[3] = m[1] * t[2] + m[3] * t[3];
    r[4] = m[0] * t[4] + m[2] * t[5] + m[4];
    r[5] = m[1] * t[4] + m[3] * t[5] + m[5];
    memcpy(m, r, sizeof(r));
}

static double scale_of(const double *m)            /* srednie powiekszenie (grubosc linii) */
{
    return js_sqrt(js_fabs(m[0] * m[3] - m[1] * m[2]));
}

/* ---------- sciezka ---------- */

static void push_pt(js_rt *rt, struct c2d *c, double x, double y)
{
    grow(rt, (void **)&c->pts, &c->pcap, c->npts + 1, sizeof(struct cpt));
    c->pts[c->npts].x = x;
    c->pts[c->npts].y = y;
    c->npts++;
    c->subs[c->nsubs - 1].n++;
}

static void dev_move(js_rt *rt, struct c2d *c, double x, double y)
{
    if (c->nsubs && c->subs[c->nsubs - 1].n == 1 && !c->subs[c->nsubs - 1].closed) {
        c->pts[c->npts - 1].x = x;                   /* samotny punkt - zastepujemy */
        c->pts[c->npts - 1].y = y;
        return;
    }
    grow(rt, (void **)&c->subs, &c->scap, c->nsubs + 1, sizeof(struct csub));
    struct csub *s = &c->subs[c->nsubs++];
    s->start = c->npts;
    s->n = 0;
    s->closed = 0;
    push_pt(rt, c, x, y);
}

static void dev_line(js_rt *rt, struct c2d *c, double x, double y)
{
    if (!c->nsubs) {
        dev_move(rt, c, x, y);
        return;
    }
    struct cpt *last = &c->pts[c->npts - 1];
    if (last->x == x && last->y == y && c->subs[c->nsubs - 1].n > 1)
        return;
    push_pt(rt, c, x, y);
}

static void user_move(js_rt *rt, struct c2d *c, double x, double y)
{
    double dx, dy;
    apply(c->st.m, x, y, &dx, &dy);
    dev_move(rt, c, dx, dy);
}

static void user_line(js_rt *rt, struct c2d *c, double x, double y)
{
    double dx, dy;
    apply(c->st.m, x, y, &dx, &dy);
    dev_line(rt, c, dx, dy);
}

static void close_path(js_rt *rt, struct c2d *c)
{
    if (!c->nsubs)
        return;
    struct csub *s = &c->subs[c->nsubs - 1];
    if (s->closed)
        return;
    s->closed = 1;
    struct cpt first = c->pts[s->start];
    grow(rt, (void **)&c->subs, &c->scap, c->nsubs + 1, sizeof(struct csub));
    struct csub *ns = &c->subs[c->nsubs++];           /* nowa sciezka zaczyna sie w poczatku starej */
    ns->start = c->npts;
    ns->n = 0;
    ns->closed = 0;
    push_pt(rt, c, first.x, first.y);
}

/* Biezacy punkt w ukladzie uzytkownika (arcTo potrzebuje). 0 = brak */
static int user_last(struct c2d *c, double *x, double *y)
{
    double inv[6];
    if (!c->nsubs || !invert(c->st.m, inv))
        return 0;
    struct cpt *p = &c->pts[c->npts - 1];
    apply(inv, p->x, p->y, x, y);
    return 1;
}

static int seg_count(double len_px, int lo, int hi)
{
    int n = (int)(len_px / 2.5) + 1;
    return n < lo ? lo : n > hi ? hi : n;
}

/* Luk elipsy (uklad uzytkownika), spec: najpierw linia do poczatku luku */
static void add_ellipse(js_rt *rt, struct c2d *c, double x, double y, double rx, double ry, double rot,
                        double a0, double a1, int ccw)
{
    double sweep, tau = 2 * PI;
    if (!ccw && a1 - a0 >= tau)
        sweep = tau;
    else if (ccw && a0 - a1 >= tau)
        sweep = -tau;
    else {
        sweep = js_fmod(a1 - a0, tau);
        if (!ccw && sweep < 0)
            sweep += tau;
        if (ccw && sweep > 0)
            sweep -= tau;
    }
    double cr = js_cos(rot), sr = js_sin(rot);
    double rmax = dmax(rx, ry) * scale_of(c->st.m);
    int n = seg_count(js_fabs(sweep) * rmax, (int)(js_fabs(sweep) / tau * 16) + 1, 2000);
    for (int i = 0; i <= n; i++) {
        double t = a0 + sweep * i / n;
        double ex = rx * js_cos(t), ey = ry * js_sin(t);
        double px = x + ex * cr - ey * sr, py = y + ex * sr + ey * cr;
        if (i == 0 && !c->nsubs)
            user_move(rt, c, px, py);
        else
            user_line(rt, c, px, py);
    }
}

static void add_rect_path(js_rt *rt, struct c2d *c, double x, double y, double w, double h)
{
    user_move(rt, c, x, y);
    user_line(rt, c, x + w, y);
    user_line(rt, c, x + w, y + h);
    user_line(rt, c, x, y + h);
    close_path(rt, c);
}

/* ---------- krawedzie i wypelnianie ---------- */

static void add_edge(js_rt *rt, struct c2d *c, double x0, double y0, double x1, double y1)
{
    if (y0 == y1 || !js_isfinite(x0 + y0 + x1 + y1))
        return;
    grow(rt, (void **)&c->e, &c->ecap, c->ne + 1, sizeof(struct cedge));
    struct cedge *e = &c->e[c->ne++];
    e->dir = y1 > y0 ? 1 : -1;
    if (y1 < y0) {
        double t = x0; x0 = x1; x1 = t;
        t = y0; y0 = y1; y1 = t;
    }
    e->x0 = x0; e->y0 = y0; e->x1 = x1; e->y1 = y1;
    e->dxdy = (x1 - x0) / (y1 - y0);
}

/* Wielokat (zamkniety) jako krawedzie. positive: odwracamy go, zeby zawsze krazyl
 * w te sama strone - wtedy nakladajace sie kawalki grubej linii sie sumuja */
static void add_poly(js_rt *rt, struct c2d *c, const struct cpt *p, int n, int positive)
{
    if (n < 2)
        return;
    int rev = 0;
    if (positive) {
        double area = 0;
        for (int i = 0; i < n; i++) {
            const struct cpt *a = &p[i], *b = &p[(i + 1) % n];
            area += a->x * b->y - b->x * a->y;
        }
        rev = area < 0;
    }
    for (int i = 0; i < n; i++) {
        const struct cpt *a = &p[i], *b = &p[(i + 1) % n];
        if (rev)
            add_edge(rt, c, b->x, b->y, a->x, a->y);
        else
            add_edge(rt, c, a->x, a->y, b->x, b->y);
    }
}

static void path_edges(js_rt *rt, struct c2d *c)
{
    for (int s = 0; s < c->nsubs; s++)
        add_poly(rt, c, &c->pts[c->subs[s].start], c->subs[s].n, 0);
}

/* Co zrobic z pokryciem wiersza: rysowanie farba, wycinanie maski... */
struct emit {
    int (*row)(struct c2d *c, struct emit *em, int y, int x0, int x1);
    struct dom_canvas *cv;
    const struct paint *paint;
    double alpha;
    int op;
    const uint8_t *mask;
    uint8_t *out_mask;
    double inv[6];
};

/* Pokrycie [xa, xb) w wierszu z waga wgt */
static void cover(float *cov, int w, double xa, double xb, double wgt, int *mn, int *mx)
{
    if (xa < 0) xa = 0;
    if (xb > w) xb = w;
    if (xb <= xa)
        return;
    int ia = ifloor(xa), ib = ifloor(xb);
    if (ia < *mn) *mn = ia;
    if (ib > *mx) *mx = ib;
    if (ia == ib) {
        cov[ia] += (float)((xb - xa) * wgt);
        return;
    }
    cov[ia] += (float)((ia + 1 - xa) * wgt);
    float fw = (float)wgt;
    for (int i = ia + 1; i < ib; i++)
        cov[i] += fw;
    if (ib < w)
        cov[ib] += (float)((xb - ib) * wgt);
}

static void sort_order(struct c2d *c)
{
    int n = c->ne, *o = c->order;
    for (int i = 0; i < n; i++)
        o[i] = i;
    for (int gap = n / 2; gap > 0; gap /= 2)        /* sortowanie Shella po gornym y */
        for (int i = gap; i < n; i++) {
            int t = o[i], j = i;
            while (j >= gap && c->e[o[j - gap]].y0 > c->e[t].y0) {
                o[j] = o[j - gap];
                j -= gap;
            }
            o[j] = t;
        }
}

/* Wypelnia krawedzie c->e (zasada nonzero albo evenodd) i oddaje pokrycie wiersz po wierszu */
static void raster(js_rt *rt, struct c2d *c, int evenodd, struct emit *em)
{
    int w = c->w, h = c->h;
    if (!c->ne || w <= 0 || h <= 0 || !c->cov)
        return;
    double ymin = 1e300, ymax = -1e300;
    for (int i = 0; i < c->ne; i++) {
        ymin = dmin(ymin, c->e[i].y0);
        ymax = dmax(ymax, c->e[i].y1);
    }
    int y0 = ifloor(dmax(0, ymin)), y1 = ifloor(dmin(h - 1, ymax));
    if (y1 < y0)
        return;
    grow(rt, (void **)&c->order, &c->ocap, c->ne * 2, sizeof(int));     /* bufor uzywany ponownie */
    int *active = c->order + c->ne, nact = 0, next = 0;
    sort_order(c);
    float *cov = c->cov;
    struct { double x; int dir; } xs[512];
    for (int y = y0; y <= y1; y++) {
        /* aktywne krawedzie: zaczynaja sie przed koncem wiersza, koncza po jego poczatku */
        while (next < c->ne && c->e[c->order[next]].y0 < y + 1)
            active[nact++] = c->order[next++];
        int k = 0;
        for (int i = 0; i < nact; i++)
            if (c->e[active[i]].y1 > y)
                active[k++] = active[i];
        nact = k;
        if (!nact)
            continue;
        int mn = w, mx = -1;
        for (int s = 0; s < SUBROWS; s++) {
            double sy = y + (s + 0.5) / SUBROWS;
            int nx = 0;
            for (int i = 0; i < nact && nx < 512; i++) {
                const struct cedge *e = &c->e[active[i]];
                if (sy < e->y0 || sy >= e->y1)
                    continue;
                xs[nx].x = e->x0 + (sy - e->y0) * e->dxdy;
                xs[nx].dir = e->dir;
                nx++;
            }
            for (int i = 1; i < nx; i++) {              /* przez wstawianie - zwykle kilka punktow */
                double tx = xs[i].x;
                int td = xs[i].dir, j = i;
                while (j > 0 && xs[j - 1].x > tx) {
                    xs[j] = xs[j - 1];
                    j--;
                }
                xs[j].x = tx;
                xs[j].dir = td;
            }
            int wind = 0;
            for (int i = 0; i < nx - 1; i++) {
                wind += xs[i].dir;
                int in = evenodd ? (wind & 1) : wind != 0;
                if (in)
                    cover(cov, w, xs[i].x, xs[i + 1].x, 1.0 / SUBROWS, &mn, &mx);
            }
        }
        if (mx >= w)
            mx = w - 1;
        if (mn <= mx) {
            em->row(c, em, y, mn, mx);
            for (int x = mn; x <= mx; x++)
                cov[x] = 0;
        }
        if (rt)
            js_step(rt);
    }
}

/* ---------- laczenie kolorow z plotnem ---------- */

static inline uint32_t pack(double r, double g, double b, double a)
{
    if (a <= 0.0005)
        return 0;
    if (a > 1) a = 1;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    return (uint32_t)(a * 255 + 0.5) << 24 | (uint32_t)(r + 0.5) << 16 | (uint32_t)(g + 0.5) << 8 | (uint32_t)(b + 0.5);
}

/* Kolor (r, g, b, a) z pokryciem k na piksel *p wedlug operacji op */
static void blend(uint32_t *p, double r, double g, double b, double a, double k, int op)
{
    uint32_t d = *p;
    double da = (d >> 24) / 255.0, dr = d >> 16 & 255, dg = d >> 8 & 255, db = d & 255;
    double sa = a * k, oa;
    switch (op) {
    case CO_OVER:
        if (sa >= 0.999 && op == CO_OVER) {
            *p = pack(r, g, b, 1);
            return;
        }
        oa = sa + da * (1 - sa);
        if (oa <= 0)
            return;
        *p = pack((r * sa + dr * da * (1 - sa)) / oa, (g * sa + dg * da * (1 - sa)) / oa,
                  (b * sa + db * da * (1 - sa)) / oa, oa);
        return;
    case CO_DEST_OVER:
        oa = da + sa * (1 - da);
        if (oa <= 0)
            return;
        *p = pack((dr * da + r * sa * (1 - da)) / oa, (dg * da + g * sa * (1 - da)) / oa,
                  (db * da + b * sa * (1 - da)) / oa, oa);
        return;
    case CO_DEST_OUT:
        *p = pack(dr, dg, db, da * (1 - sa));
        return;
    case CO_CLEAR:                                   /* clearRect: k = pokrycie */
        *p = pack(dr, dg, db, da * (1 - k));
        return;
    case CO_LIGHTER: {
        oa = dmin(1, sa + da);
        if (oa <= 0)
            return;
        *p = pack((r * sa + dr * da) / oa, (g * sa + dg * da) / oa, (b * sa + db * da) / oa, oa);
        return;
    }
    case CO_COPY: {                                  /* w obszarze ksztaltu: zamiast tla */
        double na = a * k + da * (1 - k);
        if (na <= 0) {
            *p = 0;
            return;
        }
        *p = pack((r * a * k + dr * da * (1 - k)) / na, (g * a * k + dg * da * (1 - k)) / na,
                  (b * a * k + db * da * (1 - k)) / na, na);
        return;
    }
    case CO_SRC_ATOP:
        oa = da;
        if (oa <= 0)
            return;
        *p = pack(r * sa + dr * (1 - sa), g * sa + dg * (1 - sa), b * sa + db * (1 - sa), oa);
        return;
    case CO_DEST_IN:
        *p = pack(dr, dg, db, da * (sa + (1 - k)));
        return;
    case CO_SRC_IN: {
        double na = a * da * k + da * (1 - k);
        if (na <= 0) {
            *p = 0;
            return;
        }
        *p = pack((r * a * da * k + dr * da * (1 - k)) / na, (g * a * da * k + dg * da * (1 - k)) / na,
                  (b * a * da * k + db * da * (1 - k)) / na, na);
        return;
    }
    case CO_XOR:
        oa = sa * (1 - da) + da * (1 - sa);
        if (oa <= 0) {
            *p = 0;
            return;
        }
        *p = pack((r * sa * (1 - da) + dr * da * (1 - sa)) / oa, (g * sa * (1 - da) + dg * da * (1 - sa)) / oa,
                  (b * sa * (1 - da) + db * da * (1 - sa)) / oa, oa);
        return;
    }
}

/* Tablica kolorow gradientu (256 krokow) - liczona przed kazdym rysowaniem */
static void build_lut(struct c2d *c, const struct cgrad *g)
{
    for (int i = 0; i < 256; i++) {
        double t = i / 255.0;
        double r = 0, gg = 0, b = 0, a = 0;
        if (g->n) {
            const struct gstop *lo = &g->stops[0], *hi = &g->stops[g->n - 1];
            if (t <= lo->t) {
                hi = lo;
            } else if (t >= hi->t) {
                lo = hi;
            } else {
                for (int k = 0; k + 1 < g->n; k++)
                    if (t >= g->stops[k].t && t <= g->stops[k + 1].t) {
                        lo = &g->stops[k];
                        hi = &g->stops[k + 1];
                    }
            }
            double f = hi->t > lo->t ? (t - lo->t) / (hi->t - lo->t) : 0;
            /* przejscie na kolorach pomnozonych przez alfe (jak w przegladarkach) */
            a = lo->a + (hi->a - lo->a) * f;
            if (a > 0) {
                r = (lo->r * lo->a + (hi->r * hi->a - lo->r * lo->a) * f) / a;
                gg = (lo->g * lo->a + (hi->g * hi->a - lo->g * lo->a) * f) / a;
                b = (lo->b * lo->a + (hi->b * hi->a - lo->b * lo->a) * f) / a;
            }
        }
        c->lut[i][0] = r; c->lut[i][1] = gg; c->lut[i][2] = b; c->lut[i][3] = a;
    }
}

/* Kolor gradientu w pikselu (px, py) plotna */
static const double *grad_at(struct c2d *c, const struct emit *em, double px, double py)
{
    const struct cgrad *g = em->paint->grad;
    double ux, uy, t;
    apply(em->inv, px, py, &ux, &uy);
    if (g->type == PT_LINEAR) {
        double dx = g->x1 - g->x0, dy = g->y1 - g->y0, l2 = dx * dx + dy * dy;
        t = l2 > 0 ? ((ux - g->x0) * dx + (uy - g->y0) * dy) / l2 : 0;
    } else {
        double cdx = g->x1 - g->x0, cdy = g->y1 - g->y0, dr = g->r1 - g->r0;
        double pdx = ux - g->x0, pdy = uy - g->y0;
        double A = cdx * cdx + cdy * cdy - dr * dr;
        double B = pdx * cdx + pdy * cdy + g->r0 * dr;
        double C = pdx * pdx + pdy * pdy - g->r0 * g->r0;
        if (js_fabs(A) < 1e-9) {
            if (B == 0)
                return 0;
            t = C / (2 * B);
        } else {
            double disc = B * B - A * C;
            if (disc < 0)
                return 0;
            double sq = js_sqrt(disc);
            t = (B + sq) / A;
            if (g->r0 + t * dr < 0)
                t = (B - sq) / A;
        }
        if (g->r0 + t * dr < 0)
            return 0;
    }
    int i = (int)(clamp01(t) * 255 + 0.5);
    return c->lut[i];
}

/* Wiersz pokrycia -> piksele plotna */
static int row_paint(struct c2d *c, struct emit *em, int y, int x0, int x1)
{
    uint32_t *row = em->cv->px + (size_t)y * (size_t)c->w;
    const float *cov = c->cov;
    const uint8_t *mask = em->mask ? em->mask + (size_t)y * (size_t)c->w : 0;
    const struct paint *p = em->paint;
    for (int x = x0; x <= x1; x++) {
        double k = cov[x];
        if (k <= 0.001)
            continue;
        if (k > 1)
            k = 1;
        if (mask) {
            k *= mask[x] / 255.0;
            if (k <= 0.001)
                continue;
        }
        if (em->op == CO_CLEAR) {
            blend(&row[x], 0, 0, 0, 0, k, CO_CLEAR);
            continue;
        }
        if (p->type == PT_COLOR) {
            blend(&row[x], p->r, p->g, p->b, p->a * em->alpha, k, em->op);
        } else {
            const double *g = grad_at(c, em, x + 0.5, y + 0.5);
            if (g)
                blend(&row[x], g[0], g[1], g[2], g[3] * em->alpha, k, em->op);
        }
    }
    return 0;
}

/* Wiersz pokrycia -> nowa maska obcinania (przeciecie ze stara) */
static int row_mask(struct c2d *c, struct emit *em, int y, int x0, int x1)
{
    uint8_t *out = em->out_mask + (size_t)y * (size_t)c->w;
    for (int x = x0; x <= x1; x++) {
        double k = c->cov[x];
        out[x] = (uint8_t)(k >= 1 ? 255 : k <= 0 ? 0 : (int)(k * 255 + 0.5));
    }
    return 0;
}

static const uint8_t *cur_mask(struct c2d *c)
{
    return c->st.clip >= 0 && c->mask_w == c->w && c->mask_h == c->h ? c->masks[c->st.clip] : 0;
}

/* Wypelnia gotowe krawedzie (c->e) farba */
static void fill_edges(js_rt *rt, struct c2d *c, struct dom_canvas *cv, const struct paint *p, int evenodd, int op)
{
    struct emit em;
    memset(&em, 0, sizeof(em));
    em.row = row_paint;
    em.cv = cv;
    em.paint = p;
    em.alpha = c->st.alpha;
    em.op = op;
    em.mask = cur_mask(c);
    if (p->type != PT_COLOR) {
        if (!invert(c->st.m, em.inv))
            return;
        build_lut(c, p->grad);
    }
    raster(rt, c, evenodd, &em);
    changed(rt);
}

/* ---------- grube linie (stroke) jako wielokaty ---------- */

static void circle_poly(js_rt *rt, struct c2d *c, double x, double y, double r)
{
    struct cpt p[64];
    int n = (int)(r * 1.5) + 8;
    if (n > 64)
        n = 64;
    for (int i = 0; i < n; i++) {
        double a = 2 * PI * i / n;
        p[i].x = x + r * js_cos(a);
        p[i].y = y + r * js_sin(a);
    }
    add_poly(rt, c, p, n, 1);
}

static void stroke_sub(js_rt *rt, struct c2d *c, const struct cpt *src, int n, int closed, double hw)
{
    /* bez powtorzonych punktow */
    grow(rt, (void **)&c->tmp, &c->tcap, n + 1, sizeof(struct cpt));
    struct cpt *p = c->tmp;
    int m = 0;
    for (int i = 0; i < n; i++)
        if (!m || js_fabs(src[i].x - p[m - 1].x) > 1e-9 || js_fabs(src[i].y - p[m - 1].y) > 1e-9)
            p[m++] = src[i];
    if (closed && m > 1 && js_fabs(p[0].x - p[m - 1].x) < 1e-9 && js_fabs(p[0].y - p[m - 1].y) < 1e-9)
        m--;
    if (m < 2) {
        if (m == 1 && c->st.cap == LC_ROUND)
            circle_poly(rt, c, p[0].x, p[0].y, hw);
        else if (m == 1 && c->st.cap == LC_SQUARE) {
            struct cpt q[4] = { { p[0].x - hw, p[0].y - hw }, { p[0].x + hw, p[0].y - hw },
                                { p[0].x + hw, p[0].y + hw }, { p[0].x - hw, p[0].y + hw } };
            add_poly(rt, c, q, 4, 1);
        }
        return;
    }
    int segs = closed ? m : m - 1;
    for (int i = 0; i < segs; i++) {
        const struct cpt *a = &p[i], *b = &p[(i + 1) % m];
        double dx = b->x - a->x, dy = b->y - a->y, len = js_sqrt(dx * dx + dy * dy);
        dx /= len;
        dy /= len;
        double nx = -dy * hw, ny = dx * hw;
        double s0 = 0, s1 = 0;                       /* przedluzenie (zakonczenie kwadratowe) */
        if (!closed && c->st.cap == LC_SQUARE) {
            if (i == 0) s0 = hw;
            if (i == segs - 1) s1 = hw;
        }
        struct cpt q[4] = {
            { a->x - dx * s0 + nx, a->y - dy * s0 + ny }, { b->x + dx * s1 + nx, b->y + dy * s1 + ny },
            { b->x + dx * s1 - nx, b->y + dy * s1 - ny }, { a->x - dx * s0 - nx, a->y - dy * s0 - ny },
        };
        add_poly(rt, c, q, 4, 1);
    }
    /* polaczenia odcinkow */
    for (int i = closed ? 0 : 1; i < (closed ? m : m - 1); i++) {
        const struct cpt *v = &p[i], *a = &p[(i + m - 1) % m], *b = &p[(i + 1) % m];
        if (c->st.join == LJ_ROUND) {
            circle_poly(rt, c, v->x, v->y, hw);
            continue;
        }
        double d1x = v->x - a->x, d1y = v->y - a->y, d2x = b->x - v->x, d2y = b->y - v->y;
        double l1 = js_sqrt(d1x * d1x + d1y * d1y), l2 = js_sqrt(d2x * d2x + d2y * d2y);
        d1x /= l1; d1y /= l1; d2x /= l2; d2y /= l2;
        double cross = d1x * d2y - d1y * d2x;
        if (js_fabs(cross) < 1e-9)
            continue;                                /* prosto - nic nie brakuje */
        double side = cross > 0 ? -1 : 1;            /* zewnetrzna strona zakretu */
        struct cpt pa = { v->x + side * -d1y * hw, v->y + side * d1x * hw };
        struct cpt pb = { v->x + side * -d2y * hw, v->y + side * d2x * hw };
        if (c->st.join == LJ_MITER) {
            double t = ((pb.x - pa.x) * d2y - (pb.y - pa.y) * d2x) / cross;
            struct cpt tip = { pa.x + d1x * t, pa.y + d1y * t };
            double ml = js_sqrt((tip.x - v->x) * (tip.x - v->x) + (tip.y - v->y) * (tip.y - v->y));
            if (ml <= c->st.miter * hw) {
                struct cpt q[4] = { *v, pa, tip, pb };
                add_poly(rt, c, q, 4, 1);
                continue;
            }
        }
        struct cpt q[3] = { *v, pa, pb };
        add_poly(rt, c, q, 3, 1);
    }
    if (!closed && c->st.cap == LC_ROUND) {
        circle_poly(rt, c, p[0].x, p[0].y, hw);
        circle_poly(rt, c, p[m - 1].x, p[m - 1].y, hw);
    }
}

static void stroke_path(js_rt *rt, struct c2d *c, struct dom_canvas *cv)
{
    double lw = c->st.line_width * scale_of(c->st.m);
    if (lw <= 0)
        return;
    c->ne = 0;
    for (int s = 0; s < c->nsubs; s++)
        stroke_sub(rt, c, &c->pts[c->subs[s].start], c->subs[s].n, c->subs[s].closed, lw / 2);
    fill_edges(rt, c, cv, &c->st.stroke, 0, c->st.op);
}

/* ---------- HTMLCanvasElement ---------- */

static struct dom_node *CANVAS(js_rt *rt, jv self)
{
    if (self.t != JT_OBJ || self.u.o->cls != JC_NODE || !self.u.o->u.dom.node ||
        self.u.o->u.dom.node->tag != T_CANVAS)
        js_throw_error(rt, JS_ERR_TYPE, "Illegal invocation");
    return self.u.o->u.dom.node;
}

NATIVE(cv_get_context)
{
    struct dom_node *n = CANVAS(rt, self);
    char type[16];
    js_utf8(js_to_str(rt, ARG(0)), type, sizeof(type));
    if (!lower_eq(type, "2d"))
        return jv_null();                            /* webgl itp. - nie umiemy */
    jstr *key = js_atom(rt, "__ctx2d");
    struct jprop *pr = js_own(self.u.o, key);
    if (pr && pr->v.t == JT_OBJ)
        return pr->v;
    struct c2d *c = js_alloc(rt, sizeof(*c));
    c->kind = CK_CTX;
    c->node = n;
    c->w = c->h = -1;
    reset_state(&c->st);
    jobj *o = js_new_object(rt, rt->ctx2d_proto);
    o->cls = JC_CANVAS2D;
    o->u.cv.node = n;
    o->u.cv.c = c;
    c->self = self.u.o;
    js_define(rt, self.u.o, key, jv_obj(o), 0);
    struct dom_canvas *cv = rt->doc ? dom_canvas_sync(rt->doc, n) : 0;
    if (cv)
        ctx_resized(c, cv, 0);
    return jv_obj(o);
}

static jv dim(js_rt *rt, jv self, int height)
{
    int w, h;
    dom_canvas_dims(CANVAS(rt, self), &w, &h);
    return jv_num(height ? h : w);
}

static void set_dim(js_rt *rt, jv self, int height, jv v)
{
    struct dom_node *n = CANVAS(rt, self);
    double d = js_to_num(rt, v);
    int x = js_isfinite(d) && d >= 0 && d < 100000 ? (int)d : height ? 150 : 300;
    char buf[16];
    ksnprintf(buf, sizeof(buf), "%d", x);
    if (!rt->doc)
        return;
    dom_set_attr(rt->doc, n, height ? "height" : "width", buf);
    rt->dirty = 1;                                   /* inny rozmiar na stronie */
    struct dom_canvas *cv = dom_canvas_sync(rt->doc, n);
    if (cv && cv->px)                                /* ustawienie rozmiaru zawsze czysci plotno */
        memset(cv->px, 0, (size_t)cv->w * (size_t)cv->h * 4);
    struct jprop *pr = js_own(self.u.o, js_atom(rt, "__ctx2d"));
    if (pr && pr->v.t == JT_OBJ && cv)
        ctx_resized(pr->v.u.o->u.cv.c, cv, 1);
    changed(rt);
}

NATIVE(cv_width) { return dim(rt, self, 0); }
NATIVE(cv_height) { return dim(rt, self, 1); }
NATIVE(cv_set_width) { set_dim(rt, self, 0, ARG(0)); return jv_undef(); }
NATIVE(cv_set_height) { set_dim(rt, self, 1, ARG(0)); return jv_undef(); }
NATIVE(cv_to_data_url) { return str8(rt, "data:,"); }
NATIVE(cv_noop) { return jv_undef(); }

/* img.complete, img.naturalWidth - czy obrazek jest juz pobrany */
static int img_pixels(js_rt *rt, struct dom_node *n, int *w, int *h, const uint32_t **px)
{
    return n && n->tag == T_IMG && rt->host.image && rt->host.image(rt->host.ctx, n, w, h, px);
}

static struct dom_node *node_of(jv v)
{
    return v.t == JT_OBJ && v.u.o->cls == JC_NODE ? v.u.o->u.dom.node : 0;
}

NATIVE(img_complete)
{
    int w, h;
    const uint32_t *px;
    struct dom_node *n = node_of(self);
    return jv_bool(n && img_pixels(rt, n, &w, &h, &px));
}

NATIVE(img_natural_width)
{
    int w = 0, h = 0;
    const uint32_t *px;
    struct dom_node *n = node_of(self);
    if (n && n->tag == T_CANVAS)
        dom_canvas_dims(n, &w, &h);
    else if (!img_pixels(rt, n, &w, &h, &px))
        w = 0;
    return jv_num(w);
}

NATIVE(img_natural_height)
{
    int w = 0, h = 0;
    const uint32_t *px;
    struct dom_node *n = node_of(self);
    if (n && n->tag == T_CANVAS)
        dom_canvas_dims(n, &w, &h);
    else if (!img_pixels(rt, n, &w, &h, &px))
        h = 0;
    return jv_num(h);
}

/* ---------- stan: save / restore, przeksztalcenia ---------- */

NATIVE(x_canvas) { return jv_obj(CTX(rt, self)->self); }

NATIVE(x_save)
{
    struct c2d *c = CTX(rt, self);
    if (c->sp < STACK_MAX)
        c->stack[c->sp++] = c->st;
    return jv_undef();
}

NATIVE(x_restore)
{
    struct c2d *c = CTX(rt, self);
    if (c->sp > 0)
        c->st = c->stack[--c->sp];
    return jv_undef();
}

NATIVE(x_reset)
{
    struct c2d *c = CTX(rt, self);
    reset_state(&c->st);
    c->sp = 0;
    c->npts = c->nsubs = 0;
    struct dom_canvas *cv = pixels(rt, c);
    if (cv) {
        memset(cv->px, 0, (size_t)cv->w * (size_t)cv->h * 4);
        changed(rt);
    }
    return jv_undef();
}

NATIVE(x_translate)
{
    double a[2];
    struct c2d *c = CTX(rt, self);
    if (nums(rt, argc, argv, 2, a)) {
        double t[6] = { 1, 0, 0, 1, a[0], a[1] };
        mul(c->st.m, t);
    }
    return jv_undef();
}

NATIVE(x_scale)
{
    double a[2];
    struct c2d *c = CTX(rt, self);
    if (nums(rt, argc, argv, 2, a)) {
        double t[6] = { a[0], 0, 0, a[1], 0, 0 };
        mul(c->st.m, t);
    }
    return jv_undef();
}

NATIVE(x_rotate)
{
    double a;
    struct c2d *c = CTX(rt, self);
    if (nums(rt, argc, argv, 1, &a)) {
        double co = js_cos(a), si = js_sin(a);
        double t[6] = { co, si, -si, co, 0, 0 };
        mul(c->st.m, t);
    }
    return jv_undef();
}

NATIVE(x_transform)
{
    double a[6];
    struct c2d *c = CTX(rt, self);
    if (nums(rt, argc, argv, 6, a))
        mul(c->st.m, a);
    return jv_undef();
}

NATIVE(x_set_transform)
{
    struct c2d *c = CTX(rt, self);
    double a[6] = { 1, 0, 0, 1, 0, 0 };
    if (argc == 1 && argv[0].t == JT_OBJ) {          /* setTransform({a, b, c, d, e, f}) */
        static const char *const k[6] = { "a", "b", "c", "d", "e", "f" };
        for (int i = 0; i < 6; i++) {
            jv v = js_get(rt, argv[0], js_atom(rt, k[i]));
            if (v.t != JT_UNDEF)
                a[i] = js_to_num(rt, v);
        }
        for (int i = 0; i < 6; i++)
            if (!js_isfinite(a[i]))
                return jv_undef();
    } else if (argc >= 6 && !nums(rt, argc, argv, 6, a)) {
        return jv_undef();
    }
    memcpy(c->st.m, a, sizeof(a));
    return jv_undef();
}

NATIVE(x_reset_transform)
{
    struct c2d *c = CTX(rt, self);
    double id[6] = { 1, 0, 0, 1, 0, 0 };
    memcpy(c->st.m, id, sizeof(id));
    return jv_undef();
}

NATIVE(x_get_transform)
{
    struct c2d *c = CTX(rt, self);
    jobj *o = js_new_object(rt, rt->object_proto);
    static const char *const k[6] = { "a", "b", "c", "d", "e", "f" };
    for (int i = 0; i < 6; i++)
        js_set(rt, jv_obj(o), js_atom(rt, k[i]), jv_num(c->st.m[i]));
    js_set(rt, jv_obj(o), js_atom(rt, "is2D"), jv_bool(1));
    return jv_obj(o);
}

/* ---------- wlasciwosci ---------- */

static void set_paint(js_rt *rt U, struct paint *p, jv v)
{
    if (v.t == JT_OBJ && kind_of(v.u.o) == CK_GRAD) {
        struct cgrad *g = (struct cgrad *)v.u.o->u.cv.c;
        p->type = g->type;
        p->grad = g;
        p->value = v;
        return;
    }
    if (v.t != JT_STR)
        return;                                      /* wzory (createPattern) - nie umiemy */
    char buf[96];
    js_utf8(v.u.s, buf, sizeof(buf));
    double c[4];
    if (!parse_color(buf, c))
        return;
    p->type = PT_COLOR;
    p->grad = 0;
    p->r = c[0]; p->g = c[1]; p->b = c[2]; p->a = c[3];
}

NATIVE(x_fill_style) { return color_value(rt, &CTX(rt, self)->st.fill); }
NATIVE(x_stroke_style) { return color_value(rt, &CTX(rt, self)->st.stroke); }
NATIVE(x_set_fill_style) { set_paint(rt, &CTX(rt, self)->st.fill, ARG(0)); return jv_undef(); }
NATIVE(x_set_stroke_style) { set_paint(rt, &CTX(rt, self)->st.stroke, ARG(0)); return jv_undef(); }

NATIVE(x_global_alpha) { return jv_num(CTX(rt, self)->st.alpha); }
NATIVE(x_set_global_alpha)
{
    struct c2d *c = CTX(rt, self);
    double a = js_to_num(rt, ARG(0));
    if (js_isfinite(a) && a >= 0 && a <= 1)
        c->st.alpha = a;
    return jv_undef();
}

NATIVE(x_line_width) { return jv_num(CTX(rt, self)->st.line_width); }
NATIVE(x_set_line_width)
{
    struct c2d *c = CTX(rt, self);
    double a = js_to_num(rt, ARG(0));
    if (js_isfinite(a) && a > 0)
        c->st.line_width = a;
    return jv_undef();
}

NATIVE(x_miter_limit) { return jv_num(CTX(rt, self)->st.miter); }
NATIVE(x_set_miter_limit)
{
    struct c2d *c = CTX(rt, self);
    double a = js_to_num(rt, ARG(0));
    if (js_isfinite(a) && a > 0)
        c->st.miter = a;
    return jv_undef();
}

/* Wlasciwosci-slowa: lineCap, lineJoin, textAlign... (lista w kolejnosci wartosci) */
static jv word_get(js_rt *rt, const char *const *words, int v) { return str8(rt, words[v]); }

static int word_find(js_rt *rt, const char *const *words, jv v)
{
    char buf[40];
    js_utf8(js_to_str(rt, v), buf, sizeof(buf));
    for (int i = 0; words[i]; i++)
        if (strcmp(buf, words[i]) == 0)
            return i;
    return -1;
}

static const char *const caps[] = { "butt", "round", "square", 0 };
static const char *const joins[] = { "miter", "round", "bevel", 0 };
static const char *const aligns[] = { "start", "left", "center", "right", "end", 0 };
static const char *const baselines[] = { "alphabetic", "top", "hanging", "middle", "ideographic", "bottom", 0 };
static const char *const ops[] = { "source-over", "destination-over", "destination-out", "lighter", "copy",
                                   "source-atop", "destination-in", "source-in", "xor", 0 };

NATIVE(x_line_cap) { return word_get(rt, caps, CTX(rt, self)->st.cap); }
NATIVE(x_line_join) { return word_get(rt, joins, CTX(rt, self)->st.join); }
NATIVE(x_text_align) { return word_get(rt, aligns, CTX(rt, self)->st.align); }
NATIVE(x_text_baseline) { return word_get(rt, baselines, CTX(rt, self)->st.baseline); }
NATIVE(x_comp_op) { return word_get(rt, ops, CTX(rt, self)->st.op); }

#define WORD_SETTER(name, list, field)                       \
    NATIVE(name)                                             \
    {                                                        \
        struct c2d *c = CTX(rt, self);                       \
        int i = word_find(rt, list, ARG(0));                 \
        if (i >= 0)                                          \
            c->st.field = i;                                 \
        return jv_undef();                                   \
    }
WORD_SETTER(x_set_line_cap, caps, cap)
WORD_SETTER(x_set_line_join, joins, join)
WORD_SETTER(x_set_text_align, aligns, align)
WORD_SETTER(x_set_text_baseline, baselines, baseline)

NATIVE(x_set_comp_op)
{
    struct c2d *c = CTX(rt, self);
    int i = word_find(rt, ops, ARG(0));
    if (i >= 0)
        c->st.op = i;
    else if (ARG(0).t == JT_STR)
        c->st.op = CO_OVER;                          /* multiply, screen... - rysujemy zwyczajnie */
    return jv_undef();
}

NATIVE(x_smoothing) { return jv_bool(CTX(rt, self)->st.smooth); }
NATIVE(x_set_smoothing) { CTX(rt, self)->st.smooth = js_truthy(ARG(0)); return jv_undef(); }

/* font: "bold 20px Arial", "italic 1.5em serif", "12pt sans-serif" */
static double face_em(int face) { return font_ascent(face) / 0.93; }

NATIVE(x_font)
{
    struct c2d *c = CTX(rt, self);
    return c->st.font ? jv_str(c->st.font) : str8(rt, "10px sans-serif");
}

NATIVE(x_set_font)
{
    struct c2d *c = CTX(rt, self);
    if (ARG(0).t != JT_STR)
        return jv_undef();
    char buf[160];
    js_utf8(argv[0].u.s, buf, sizeof(buf));
    int bold = 0, italic = 0;
    double px = -1;
    for (const char *s = buf; *s;) {
        s = skip_ws(s);
        const char *w = s;
        while (*s && *s != ' ' && *s != '\t')
            s++;
        char tok[40];
        unsigned l = (unsigned)(s - w) < sizeof(tok) - 1 ? (unsigned)(s - w) : sizeof(tok) - 1;
        memcpy(tok, w, l);
        tok[l] = 0;
        if (lower_eq(tok, "bold") || lower_eq(tok, "bolder") || strcmp(tok, "600") == 0 ||
            strcmp(tok, "700") == 0 || strcmp(tok, "800") == 0 || strcmp(tok, "900") == 0)
            bold = 1;
        else if (lower_eq(tok, "italic") || lower_eq(tok, "oblique"))
            italic = 1;
        else if (px < 0 && ((tok[0] >= '0' && tok[0] <= '9') || tok[0] == '.')) {
            const char *end;
            double v = js_strtod(tok, &end);
            if (end[0] == 'p' && end[1] == 'x') px = v;
            else if (end[0] == 'p' && end[1] == 't') px = v * 4 / 3;
            else if (end[0] == 'e' && end[1] == 'm') px = v * 16;
            else if (end[0] == 'r' && end[1] == 'e' && end[2] == 'm') px = v * 16;
            else if (end[0] == '%') px = v * 16 / 100;
        }
    }
    if (px <= 0)
        return jv_undef();                           /* bez rozmiaru - zly zapis, spec: ignorujemy */
    c->st.font = argv[0].u.s;
    c->st.font_px = px;
    if (bold)
        c->st.face = FONT_BOLD;
    else if (italic)
        c->st.face = FONT_ITALIC;
    else {
        static const int faces[] = { FONT_SMALL, FONT_REGULAR, FONT_H2, FONT_H1 };
        c->st.face = FONT_H1;
        for (int i = 0; i < 4; i++)                  /* najmniejsza czcionka nie mniejsza niz trzeba */
            if (face_em(faces[i]) >= px * 0.95) {
                c->st.face = faces[i];
                break;
            }
    }
    return jv_undef();
}

/* ---------- prostokaty ---------- */

/* Prostokat bez przeksztalcenia z calkowitymi brzegami - szybko, piksel po pikselu.
 * 1 = zrobione */
static int fast_rect(struct c2d *c, struct dom_canvas *cv, const double *r, const struct paint *p, int op)
{
    const double *m = c->st.m;
    if (m[1] != 0 || m[2] != 0 || (p && p->type != PT_COLOR))
        return 0;
    double x0 = m[0] * r[0] + m[4], y0 = m[3] * r[1] + m[5];
    double x1 = m[0] * (r[0] + r[2]) + m[4], y1 = m[3] * (r[1] + r[3]) + m[5];
    if (x0 > x1) { double t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { double t = y0; y0 = y1; y1 = t; }
    if (x0 != (int)x0 || x1 != (int)x1 || y0 != (int)y0 || y1 != (int)y1)
        return 0;
    int ix0 = x0 < 0 ? 0 : (int)x0, iy0 = y0 < 0 ? 0 : (int)y0;
    int ix1 = x1 > c->w ? c->w : (int)x1, iy1 = y1 > c->h ? c->h : (int)y1;
    const uint8_t *mask = cur_mask(c);
    for (int y = iy0; y < iy1; y++) {
        uint32_t *row = cv->px + (size_t)y * (size_t)c->w;
        const uint8_t *mr = mask ? mask + (size_t)y * (size_t)c->w : 0;
        if (!mr && op == CO_CLEAR) {
            memset(row + ix0, 0, (size_t)(ix1 > ix0 ? ix1 - ix0 : 0) * 4);
            continue;
        }
        if (!mr && op == CO_OVER && p->a * c->st.alpha >= 1) {
            uint32_t col = pack(p->r, p->g, p->b, 1);
            for (int x = ix0; x < ix1; x++)
                row[x] = col;
            continue;
        }
        for (int x = ix0; x < ix1; x++) {
            double k = mr ? mr[x] / 255.0 : 1;
            if (k <= 0)
                continue;
            if (op == CO_CLEAR)
                blend(&row[x], 0, 0, 0, 0, k, CO_CLEAR);
            else
                blend(&row[x], p->r, p->g, p->b, p->a * c->st.alpha, k, op);
        }
    }
    return 1;
}

static void rect_edges(js_rt *rt, struct c2d *c, const double *r)
{
    struct cpt q[4];
    double xs[4] = { r[0], r[0] + r[2], r[0] + r[2], r[0] };
    double ys[4] = { r[1], r[1], r[1] + r[3], r[1] + r[3] };
    for (int i = 0; i < 4; i++)
        apply(c->st.m, xs[i], ys[i], &q[i].x, &q[i].y);
    c->ne = 0;
    add_poly(rt, c, q, 4, 0);
}

NATIVE(x_fill_rect)
{
    double r[4];
    struct c2d *c = CTX(rt, self);
    struct dom_canvas *cv = pixels(rt, c);
    if (!cv || !nums(rt, argc, argv, 4, r) || !r[2] || !r[3])
        return jv_undef();
    if (fast_rect(c, cv, r, &c->st.fill, c->st.op)) {
        changed(rt);
        return jv_undef();
    }
    rect_edges(rt, c, r);
    fill_edges(rt, c, cv, &c->st.fill, 0, c->st.op);
    return jv_undef();
}

NATIVE(x_clear_rect)
{
    double r[4];
    struct c2d *c = CTX(rt, self);
    struct dom_canvas *cv = pixels(rt, c);
    if (!cv || !nums(rt, argc, argv, 4, r) || !r[2] || !r[3])
        return jv_undef();
    if (fast_rect(c, cv, r, 0, CO_CLEAR)) {
        changed(rt);
        return jv_undef();
    }
    rect_edges(rt, c, r);
    struct paint none;
    memset(&none, 0, sizeof(none));
    fill_edges(rt, c, cv, &none, 0, CO_CLEAR);
    return jv_undef();
}

NATIVE(x_stroke_rect)
{
    double r[4];
    struct c2d *c = CTX(rt, self);
    struct dom_canvas *cv = pixels(rt, c);
    if (!cv || !nums(rt, argc, argv, 4, r))
        return jv_undef();
    /* osobny prostokat - biezaca sciezka zostaje nietknieta */
    struct cpt q[4];
    double xs[4] = { r[0], r[0] + r[2], r[0] + r[2], r[0] };
    double ys[4] = { r[1], r[1], r[1] + r[3], r[1] + r[3] };
    for (int i = 0; i < 4; i++)
        apply(c->st.m, xs[i], ys[i], &q[i].x, &q[i].y);
    double lw = c->st.line_width * scale_of(c->st.m);
    c->ne = 0;
    stroke_sub(rt, c, q, 4, 1, lw / 2);
    fill_edges(rt, c, cv, &c->st.stroke, 0, c->st.op);
    return jv_undef();
}

/* ---------- sciezki ---------- */

NATIVE(x_begin_path)
{
    struct c2d *c = CTX(rt, self);
    c->npts = c->nsubs = 0;
    return jv_undef();
}

NATIVE(x_close_path) { close_path(rt, CTX(rt, self)); return jv_undef(); }

NATIVE(x_move_to)
{
    double a[2];
    struct c2d *c = CTX(rt, self);
    if (nums(rt, argc, argv, 2, a))
        user_move(rt, c, a[0], a[1]);
    return jv_undef();
}

NATIVE(x_line_to)
{
    double a[2];
    struct c2d *c = CTX(rt, self);
    if (nums(rt, argc, argv, 2, a))
        user_line(rt, c, a[0], a[1]);
    return jv_undef();
}

NATIVE(x_rect)
{
    double a[4];
    struct c2d *c = CTX(rt, self);
    if (nums(rt, argc, argv, 4, a))
        add_rect_path(rt, c, a[0], a[1], a[2], a[3]);
    return jv_undef();
}

NATIVE(x_arc)
{
    double a[5];
    struct c2d *c = CTX(rt, self);
    if (!nums(rt, argc, argv, 5, a))
        return jv_undef();
    if (a[2] < 0)
        js_throw_error(rt, JS_ERR_RANGE, "Failed to execute 'arc' on 'CanvasRenderingContext2D': The radius provided (%d) is negative.", (int)a[2]);
    add_ellipse(rt, c, a[0], a[1], a[2], a[2], 0, a[3], a[4], js_truthy(ARG(5)));
    return jv_undef();
}

NATIVE(x_ellipse)
{
    double a[7];
    struct c2d *c = CTX(rt, self);
    if (!nums(rt, argc, argv, 7, a))
        return jv_undef();
    if (a[2] < 0 || a[3] < 0)
        js_throw_error(rt, JS_ERR_RANGE, "Failed to execute 'ellipse' on 'CanvasRenderingContext2D': The radius provided is negative.");
    add_ellipse(rt, c, a[0], a[1], a[2], a[3], a[4], a[5], a[6], js_truthy(ARG(7)));
    return jv_undef();
}

NATIVE(x_arc_to)
{
    double a[5];
    struct c2d *c = CTX(rt, self);
    if (!nums(rt, argc, argv, 5, a))
        return jv_undef();
    if (a[4] < 0)
        js_throw_error(rt, JS_ERR_RANGE, "Failed to execute 'arcTo' on 'CanvasRenderingContext2D': The radius provided is negative.");
    double x0, y0, x1 = a[0], y1 = a[1], x2 = a[2], y2 = a[3], r = a[4];
    if (!user_last(c, &x0, &y0)) {
        user_move(rt, c, x1, y1);
        return jv_undef();
    }
    double v1x = x0 - x1, v1y = y0 - y1, v2x = x2 - x1, v2y = y2 - y1;
    double l1 = js_sqrt(v1x * v1x + v1y * v1y), l2 = js_sqrt(v2x * v2x + v2y * v2y);
    double cross = v1x * v2y - v1y * v2x;
    if (l1 < 1e-9 || l2 < 1e-9 || r == 0 || js_fabs(cross) < 1e-9 * l1 * l2) {
        user_line(rt, c, x1, y1);
        return jv_undef();
    }
    v1x /= l1; v1y /= l1; v2x /= l2; v2y /= l2;
    double cosang = v1x * v2x + v1y * v2y;
    double ang = js_acos(cosang < -1 ? -1 : cosang > 1 ? 1 : cosang);
    double dist = r / js_tan(ang / 2);
    double t1x = x1 + v1x * dist, t1y = y1 + v1y * dist, t2x = x1 + v2x * dist, t2y = y1 + v2y * dist;
    double bx = v1x + v2x, by = v1y + v2y, bl = js_sqrt(bx * bx + by * by);
    double cd = r / js_sin(ang / 2);
    double cx = x1 + bx / bl * cd, cy = y1 + by / bl * cd;
    user_line(rt, c, t1x, t1y);
    double a0 = js_atan2(t1y - cy, t1x - cx), a1 = js_atan2(t2y - cy, t2x - cx);
    add_ellipse(rt, c, cx, cy, r, r, 0, a0, a1, cross > 0);
    return jv_undef();
}

NATIVE(x_quad_to)
{
    double a[4];
    struct c2d *c = CTX(rt, self);
    if (!nums(rt, argc, argv, 4, a))
        return jv_undef();
    if (!c->nsubs)
        user_move(rt, c, a[0], a[1]);
    struct cpt p0 = c->pts[c->npts - 1], p1, p2;
    apply(c->st.m, a[0], a[1], &p1.x, &p1.y);
    apply(c->st.m, a[2], a[3], &p2.x, &p2.y);
    double len = js_sqrt((p1.x - p0.x) * (p1.x - p0.x) + (p1.y - p0.y) * (p1.y - p0.y)) +
                 js_sqrt((p2.x - p1.x) * (p2.x - p1.x) + (p2.y - p1.y) * (p2.y - p1.y));
    int n = seg_count(len, 2, 200);
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        dev_line(rt, c, u * u * p0.x + 2 * u * t * p1.x + t * t * p2.x, u * u * p0.y + 2 * u * t * p1.y + t * t * p2.y);
    }
    return jv_undef();
}

NATIVE(x_bezier_to)
{
    double a[6];
    struct c2d *c = CTX(rt, self);
    if (!nums(rt, argc, argv, 6, a))
        return jv_undef();
    if (!c->nsubs)
        user_move(rt, c, a[0], a[1]);
    struct cpt p0 = c->pts[c->npts - 1], p1, p2, p3;
    apply(c->st.m, a[0], a[1], &p1.x, &p1.y);
    apply(c->st.m, a[2], a[3], &p2.x, &p2.y);
    apply(c->st.m, a[4], a[5], &p3.x, &p3.y);
    double len = js_sqrt((p1.x - p0.x) * (p1.x - p0.x) + (p1.y - p0.y) * (p1.y - p0.y)) +
                 js_sqrt((p2.x - p1.x) * (p2.x - p1.x) + (p2.y - p1.y) * (p2.y - p1.y)) +
                 js_sqrt((p3.x - p2.x) * (p3.x - p2.x) + (p3.y - p2.y) * (p3.y - p2.y));
    int n = seg_count(len, 3, 300);
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        double b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
        dev_line(rt, c, b0 * p0.x + b1 * p1.x + b2 * p2.x + b3 * p3.x, b0 * p0.y + b1 * p1.y + b2 * p2.y + b3 * p3.y);
    }
    return jv_undef();
}

/* roundRect(x, y, w, h, r) - r: liczba albo tablica 1-4 promieni */
NATIVE(x_round_rect)
{
    double a[4];
    struct c2d *c = CTX(rt, self);
    if (!nums(rt, argc, argv, 4, a))
        return jv_undef();
    double rr[4] = { 0, 0, 0, 0 };                   /* lewy gorny, prawy gorny, prawy dolny, lewy dolny */
    jv rv = ARG(4);
    double list[4];
    int n = 0;
    if (rv.t == JT_OBJ && rv.u.o->cls == JC_ARRAY) {
        for (uint32_t i = 0; i < rv.u.o->u.arr.len && n < 4; i++)
            list[n++] = js_to_num(rt, rv.u.o->u.arr.v[i]);
    } else if (rv.t != JT_UNDEF) {
        list[n++] = js_to_num(rt, rv);
    }
    if (n == 1) { rr[0] = rr[1] = rr[2] = rr[3] = list[0]; }
    else if (n == 2) { rr[0] = rr[2] = list[0]; rr[1] = rr[3] = list[1]; }
    else if (n == 3) { rr[0] = list[0]; rr[1] = rr[3] = list[1]; rr[2] = list[2]; }
    else if (n == 4) { for (int i = 0; i < 4; i++) rr[i] = list[i]; }
    for (int i = 0; i < 4; i++)
        if (!js_isfinite(rr[i]) || rr[i] < 0)
            js_throw_error(rt, JS_ERR_RANGE, "Failed to execute 'roundRect' on 'CanvasRenderingContext2D': The radius provided is negative.");
    double x = a[0], y = a[1], w = a[2], h = a[3];
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    /* promienie za duze na boki - zmniejszamy wszystkie razem */
    double f = 1;
    if (rr[0] + rr[1] > w) f = dmin(f, w / (rr[0] + rr[1]));
    if (rr[3] + rr[2] > w) f = dmin(f, w / (rr[3] + rr[2]));
    if (rr[0] + rr[3] > h) f = dmin(f, h / (rr[0] + rr[3]));
    if (rr[1] + rr[2] > h) f = dmin(f, h / (rr[1] + rr[2]));
    for (int i = 0; i < 4; i++)
        rr[i] *= f;
    user_move(rt, c, x + rr[0], y);
    user_line(rt, c, x + w - rr[1], y);
    if (rr[1] > 0) add_ellipse(rt, c, x + w - rr[1], y + rr[1], rr[1], rr[1], 0, -PI / 2, 0, 0);
    user_line(rt, c, x + w, y + h - rr[2]);
    if (rr[2] > 0) add_ellipse(rt, c, x + w - rr[2], y + h - rr[2], rr[2], rr[2], 0, 0, PI / 2, 0);
    user_line(rt, c, x + rr[3], y + h);
    if (rr[3] > 0) add_ellipse(rt, c, x + rr[3], y + h - rr[3], rr[3], rr[3], 0, PI / 2, PI, 0);
    user_line(rt, c, x, y + rr[0]);
    if (rr[0] > 0) add_ellipse(rt, c, x + rr[0], y + rr[0], rr[0], rr[0], 0, PI, PI * 1.5, 0);
    close_path(rt, c);
    return jv_undef();
}

static int fill_rule(js_rt *rt U, jv v)
{
    if (v.t != JT_STR)
        return 0;
    char buf[16];
    js_utf8(v.u.s, buf, sizeof(buf));
    return strcmp(buf, "evenodd") == 0;
}

NATIVE(x_fill)
{
    struct c2d *c = CTX(rt, self);
    struct dom_canvas *cv = pixels(rt, c);
    if (!cv)
        return jv_undef();
    c->ne = 0;
    path_edges(rt, c);
    fill_edges(rt, c, cv, &c->st.fill, fill_rule(rt, ARG(0)), c->st.op);
    return jv_undef();
}

NATIVE(x_stroke)
{
    struct c2d *c = CTX(rt, self);
    struct dom_canvas *cv = pixels(rt, c);
    if (cv)
        stroke_path(rt, c, cv);
    return jv_undef();
}

/* clip(): nowa maska = stara maska * pokrycie sciezki */
NATIVE(x_clip)
{
    struct c2d *c = CTX(rt, self);
    struct dom_canvas *cv = pixels(rt, c);
    if (!cv)
        return jv_undef();
    size_t size = (size_t)c->w * (size_t)c->h;
    if (c->mask_w != c->w || c->mask_h != c->h) {    /* inny rozmiar plotna - maski od nowa */
        memset(c->masks, 0, sizeof(c->masks));
        c->mask_w = c->w;
        c->mask_h = c->h;
    }
    int level = c->sp;
    if (!c->masks[level])
        c->masks[level] = js_alloc_raw(rt, size);
    uint8_t *out = c->masks[level];
    const uint8_t *old = cur_mask(c);
    /* pokrycie sciezki do maski tymczasowej, potem mnozymy przez stara */
    uint8_t *tmp = kzalloc(size);
    if (!tmp)
        return jv_undef();
    struct emit em;
    memset(&em, 0, sizeof(em));
    em.row = row_mask;
    em.out_mask = tmp;
    c->ne = 0;
    path_edges(rt, c);
    raster(rt, c, fill_rule(rt, ARG(0)), &em);
    for (size_t i = 0; i < size; i++)
        out[i] = old ? (uint8_t)((tmp[i] * old[i] + 127) / 255) : tmp[i];
    kfree(tmp);
    c->st.clip = level;
    return jv_undef();
}

/* Czy punkt jest w sciezce - gry tak sprawdzaja klikniecia */
NATIVE(x_is_point_in_path)
{
    double a[2];
    struct c2d *c = CTX(rt, self);
    if (!nums(rt, argc, argv, 2, a))
        return jv_bool(0);
    double px = a[0], py = a[1];                     /* w pikselach plotna (bez przeksztalcenia) */
    int evenodd = fill_rule(rt, ARG(2)), wind = 0;
    for (int s = 0; s < c->nsubs; s++) {
        const struct cpt *p = &c->pts[c->subs[s].start];
        int n = c->subs[s].n;
        for (int i = 0; i < n && n > 1; i++) {
            const struct cpt *u = &p[i], *v = &p[(i + 1) % n];
            if ((u->y <= py) != (v->y <= py)) {
                double x = u->x + (py - u->y) * (v->x - u->x) / (v->y - u->y);
                if (x > px)
                    wind += v->y > u->y ? 1 : -1;
            }
        }
    }
    return jv_bool(evenodd ? (wind & 1) : wind != 0);
}

NATIVE(x_false) { return jv_bool(0); }
NATIVE(x_noop) { return jv_undef(); }
NATIVE(x_get_line_dash) { return jv_obj(js_new_array(rt, 0)); }

/* ---------- gradienty ---------- */

static jobj *grad_proto(js_rt *rt)
{
    struct jprop *p = js_own(rt->ctx2d_proto, js_atom(rt, "__gradproto"));
    return p && p->v.t == JT_OBJ ? p->v.u.o : rt->object_proto;
}

static jv new_grad(js_rt *rt, int type, const double *a)
{
    struct cgrad *g = js_alloc(rt, sizeof(*g));
    g->kind = CK_GRAD;
    g->type = type;
    if (type == PT_LINEAR) {
        g->x0 = a[0]; g->y0 = a[1]; g->x1 = a[2]; g->y1 = a[3];
    } else {
        g->x0 = a[0]; g->y0 = a[1]; g->r0 = a[2]; g->x1 = a[3]; g->y1 = a[4]; g->r1 = a[5];
    }
    jobj *o = js_new_object(rt, grad_proto(rt));
    o->cls = JC_CANVAS2D;
    o->u.cv.c = (struct c2d *)g;
    return jv_obj(o);
}

NATIVE(x_linear_gradient)
{
    double a[4];
    CTX(rt, self);
    if (!nums(rt, argc, argv, 4, a))
        js_throw_error(rt, JS_ERR_TYPE, "Failed to execute 'createLinearGradient': The provided double value is non-finite.");
    return new_grad(rt, PT_LINEAR, a);
}

NATIVE(x_radial_gradient)
{
    double a[6];
    CTX(rt, self);
    if (!nums(rt, argc, argv, 6, a))
        js_throw_error(rt, JS_ERR_TYPE, "Failed to execute 'createRadialGradient': The provided double value is non-finite.");
    if (a[2] < 0 || a[5] < 0)
        js_throw_error(rt, JS_ERR_RANGE, "Failed to execute 'createRadialGradient': The radius provided is negative.");
    return new_grad(rt, PT_RADIAL, a);
}

NATIVE(x_create_pattern) { return jv_null(); }

NATIVE(g_add_color_stop)
{
    if (self.t != JT_OBJ || kind_of(self.u.o) != CK_GRAD)
        js_throw_error(rt, JS_ERR_TYPE, "Illegal invocation");
    struct cgrad *g = (struct cgrad *)self.u.o->u.cv.c;
    double t = js_to_num(rt, ARG(0)), col[4];
    if (!js_isfinite(t) || t < 0 || t > 1)
        js_throw_error(rt, JS_ERR_RANGE, "Failed to execute 'addColorStop' on 'CanvasGradient': The provided value is outside the range [0.0, 1.0].");
    char buf[96];
    js_utf8(js_to_str(rt, ARG(1)), buf, sizeof(buf));
    if (!parse_color(buf, col))
        js_throw_error(rt, JS_ERR_SYNTAX, "Failed to execute 'addColorStop' on 'CanvasGradient': The value provided could not be parsed as a color.");
    grow(rt, (void **)&g->stops, &g->cap, g->n + 1, sizeof(struct gstop));
    int i = g->n++;                                  /* rosnaco po t (rowne - w kolejnosci dodania) */
    while (i > 0 && g->stops[i - 1].t > t) {
        g->stops[i] = g->stops[i - 1];
        i--;
    }
    g->stops[i].t = t;
    g->stops[i].r = col[0]; g->stops[i].g = col[1]; g->stops[i].b = col[2]; g->stops[i].a = col[3];
    return jv_undef();
}

/* ---------- tekst ---------- */

struct text_run {
    double s;                                        /* powiekszenie czcionki */
    double sx;                                       /* dodatkowe scisniecie (maxWidth) */
    double width;                                    /* szerokosc w ukladzie uzytkownika */
};

static double text_width_face(int face, const jstr *t)
{
    double w = 0;
    for (uint32_t i = 0; i < t->len; i++) {
        uint32_t cp = t->c[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < t->len) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (t->c[i + 1] - 0xDC00);
            i++;
        }
        if (cp == '\n' || cp == '\t' || cp == '\r')
            cp = ' ';
        const struct font_glyph *g = font_glyph(face, cp);
        if (g)
            w += g->adv;
    }
    return w;
}

static void draw_text(js_rt *rt, struct c2d *c, struct dom_canvas *cv, jstr *t, double x, double y,
                      double max_w, const struct paint *p)
{
    int face = c->st.face;
    double s = c->st.font_px / face_em(face);
    double wf = text_width_face(face, t);
    double sx = 1;
    if (max_w > 0 && wf * s > max_w)
        sx = max_w / (wf * s);
    double asc = font_ascent(face), desc = font_height(face) - asc;
    double ox = 0;                                   /* przesuniecie w jednostkach czcionki */
    int al = c->st.align;
    if (al == TA_RIGHT || al == TA_END) ox = -wf;
    else if (al == TA_CENTER) ox = -wf / 2;
    double oy = 0;
    switch (c->st.baseline) {
    case TB_TOP: case TB_HANGING: oy = asc; break;
    case TB_MIDDLE: oy = (asc - desc) / 2; break;
    case TB_BOTTOM: case TB_IDEOGRAPHIC: oy = -desc; break;
    }
    /* uklad czcionki (piksele bitmap) -> uklad uzytkownika -> plotno */
    double tm[6] = { s * sx, 0, 0, s, x + ox * s * sx, y + oy * s };
    double m[6];
    memcpy(m, c->st.m, sizeof(m));
    mul(m, tm);
    double inv[6];
    if (!invert(m, inv))
        return;
    struct emit em;
    memset(&em, 0, sizeof(em));
    em.paint = p;
    em.alpha = c->st.alpha;
    if (p->type != PT_COLOR) {
        if (!invert(c->st.m, em.inv))
            return;
        build_lut(c, p->grad);
    }
    const uint8_t *mask = cur_mask(c);
    const struct font_face *ff = &font_faces[face];
    double pen = 0;
    for (uint32_t i = 0; i < t->len; i++) {
        uint32_t cp = t->c[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < t->len) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (t->c[i + 1] - 0xDC00);
            i++;
        }
        if (cp == '\n' || cp == '\t' || cp == '\r')
            cp = ' ';
        const struct font_glyph *g = font_glyph(face, cp);
        if (!g)
            continue;
        if (g->w && g->h) {
            /* bitmapa znaku: od (gx, gy) w ukladzie czcionki, linia bazowa na y = 0 */
            double gx = pen + g->xoff, gy = (double)g->yoff - asc;
            double cx[4] = { gx, gx + g->w, gx + g->w, gx }, cy[4] = { gy, gy, gy + g->h, gy + g->h };
            double bx0 = 1e300, by0 = 1e300, bx1 = -1e300, by1 = -1e300;
            for (int k = 0; k < 4; k++) {
                double dx, dy;
                apply(m, cx[k], cy[k], &dx, &dy);
                bx0 = dmin(bx0, dx); by0 = dmin(by0, dy);
                bx1 = dmax(bx1, dx); by1 = dmax(by1, dy);
            }
            int ix0 = ifloor(dmax(0, bx0 - 1)), iy0 = ifloor(dmax(0, by0 - 1));
            int ix1 = ifloor(dmin(c->w - 1, bx1 + 1)), iy1 = ifloor(dmin(c->h - 1, by1 + 1));
            const uint8_t *al8 = ff->alpha + g->off;
            for (int py = iy0; py <= iy1; py++) {
                uint32_t *row = cv->px + (size_t)py * (size_t)c->w;
                for (int px = ix0; px <= ix1; px++) {
                    double u, v;
                    apply(inv, px + 0.5, py + 0.5, &u, &v);
                    u -= gx + 0.5;                   /* srodki pikseli bitmapy */
                    v -= gy + 0.5;
                    if (u <= -1 || v <= -1 || u >= g->w || v >= g->h)
                        continue;
                    int iu = ifloor(u), iv = ifloor(v);
                    double fu = u - iu, fv = v - iv, k = 0;
                    for (int dyy = 0; dyy < 2; dyy++)    /* dwuliniowo - gladko przy powiekszeniu */
                        for (int dxx = 0; dxx < 2; dxx++) {
                            int su = iu + dxx, sv = iv + dyy;
                            if (su < 0 || sv < 0 || su >= g->w || sv >= g->h)
                                continue;
                            double wgt = (dxx ? fu : 1 - fu) * (dyy ? fv : 1 - fv);
                            k += wgt * al8[sv * g->w + su];
                        }
                    k /= 255;
                    if (mask)
                        k *= mask[(size_t)py * (size_t)c->w + px] / 255.0;
                    if (k <= 0.002)
                        continue;
                    if (p->type == PT_COLOR) {
                        blend(&row[px], p->r, p->g, p->b, p->a * em.alpha, k, c->st.op);
                    } else {
                        const double *gc = grad_at(c, &em, px + 0.5, py + 0.5);
                        if (gc)
                            blend(&row[px], gc[0], gc[1], gc[2], gc[3] * em.alpha, k, c->st.op);
                    }
                }
            }
        }
        pen += g->adv;
        js_step(rt);
    }
    changed(rt);
}

static void text_op(js_rt *rt, jv self, int argc, jv *argv, int stroke)
{
    struct c2d *c = CTX(rt, self);
    struct dom_canvas *cv = pixels(rt, c);
    double a[2];
    if (!cv || !nums(rt, argc - 1 > 0 ? argc - 1 : 0, argv + 1, 2, a))
        return;
    jstr *t = js_to_str(rt, ARG(0));
    double max_w = -1;
    if (argc > 3) {
        max_w = js_to_num(rt, argv[3]);
        if (!js_isfinite(max_w) || max_w <= 0)
            return;
    }
    draw_text(rt, c, cv, t, a[0], a[1], max_w, stroke ? &c->st.stroke : &c->st.fill);
}

NATIVE(x_fill_text) { text_op(rt, self, argc, argv, 0); return jv_undef(); }
NATIVE(x_stroke_text) { text_op(rt, self, argc, argv, 1); return jv_undef(); }

NATIVE(x_measure_text)
{
    struct c2d *c = CTX(rt, self);
    jstr *t = js_to_str(rt, ARG(0));
    int face = c->st.face;
    double s = c->st.font_px / face_em(face);
    double w = text_width_face(face, t) * s;
    double asc = font_ascent(face) * s, desc = (font_height(face) - font_ascent(face)) * s;
    jobj *o = js_new_object(rt, rt->object_proto);
    jv ov = jv_obj(o);
    js_set(rt, ov, js_atom(rt, "width"), jv_num(w));
    js_set(rt, ov, js_atom(rt, "actualBoundingBoxLeft"), jv_num(0));
    js_set(rt, ov, js_atom(rt, "actualBoundingBoxRight"), jv_num(w));
    js_set(rt, ov, js_atom(rt, "actualBoundingBoxAscent"), jv_num(asc));
    js_set(rt, ov, js_atom(rt, "actualBoundingBoxDescent"), jv_num(desc));
    js_set(rt, ov, js_atom(rt, "fontBoundingBoxAscent"), jv_num(asc));
    js_set(rt, ov, js_atom(rt, "fontBoundingBoxDescent"), jv_num(desc));
    return ov;
}

/* ---------- obrazki ---------- */

/* Zrodlo dla drawImage: <img> (pobrany), <canvas> albo nic */
struct src_img {
    int w, h;
    const uint32_t *px;
    int alpha;                                       /* piksele z alfa (plotno) */
};

static int image_source(js_rt *rt, jv v, struct src_img *s)
{
    struct dom_node *n = node_of(v);
    if (!n)
        js_throw_error(rt, JS_ERR_TYPE, "Failed to execute 'drawImage' on 'CanvasRenderingContext2D': The provided value is not of type 'CanvasImageSource'.");
    if (n->tag == T_CANVAS) {
        if (!n->canvas || !n->canvas->px)
            return 0;
        s->w = n->canvas->w;
        s->h = n->canvas->h;
        s->px = n->canvas->px;
        s->alpha = 1;
        return 1;
    }
    s->alpha = 0;
    return img_pixels(rt, n, &s->w, &s->h, &s->px);
}

static uint32_t sample(const struct src_img *s, int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= s->w) x = s->w - 1;
    if (y >= s->h) y = s->h - 1;
    uint32_t p = s->px[(size_t)y * (size_t)s->w + x];
    return s->alpha ? p : p | 0xFF000000u;
}

NATIVE(x_draw_image)
{
    struct c2d *c = CTX(rt, self);
    struct dom_canvas *cv = pixels(rt, c);
    struct src_img src;
    if (!image_source(rt, ARG(0), &src) || !cv || src.w <= 0 || src.h <= 0)
        return jv_undef();
    double a[8];
    double sx = 0, sy = 0, sw = src.w, sh = src.h, dx, dy, dw = src.w, dh = src.h;
    if (argc >= 9) {
        if (!nums(rt, 8, argv + 1, 8, a))
            return jv_undef();
        sx = a[0]; sy = a[1]; sw = a[2]; sh = a[3]; dx = a[4]; dy = a[5]; dw = a[6]; dh = a[7];
    } else if (argc >= 5) {
        if (!nums(rt, 4, argv + 1, 4, a))
            return jv_undef();
        dx = a[0]; dy = a[1]; dw = a[2]; dh = a[3];
    } else {
        if (!nums(rt, argc - 1 > 0 ? argc - 1 : 0, argv + 1, 2, a))
            return jv_undef();
        dx = a[0]; dy = a[1];
    }
    if (sw < 0) { sx += sw; sw = -sw; }
    if (sh < 0) { sy += sh; sh = -sh; }
    if (dw < 0) { dx += dw; dw = -dw; }
    if (dh < 0) { dy += dh; dh = -dh; }
    if (!sw || !sh || !dw || !dh)
        return jv_undef();
    /* rysowanie plotna na samym sobie - najpierw kopia */
    uint32_t *copy = 0;
    if (src.px == cv->px) {
        copy = kmalloc((size_t)src.w * (size_t)src.h * 4);
        if (!copy)
            return jv_undef();
        memcpy(copy, src.px, (size_t)src.w * (size_t)src.h * 4);
        src.px = copy;
    }
    double m[6], inv[6];
    memcpy(m, c->st.m, sizeof(m));
    if (!invert(m, inv)) {
        kfree(copy);
        return jv_undef();
    }
    double bx0 = 1e300, by0 = 1e300, bx1 = -1e300, by1 = -1e300;
    double cx[4] = { dx, dx + dw, dx + dw, dx }, cy[4] = { dy, dy, dy + dh, dy + dh };
    for (int k = 0; k < 4; k++) {
        double X, Y;
        apply(m, cx[k], cy[k], &X, &Y);
        bx0 = dmin(bx0, X); by0 = dmin(by0, Y);
        bx1 = dmax(bx1, X); by1 = dmax(by1, Y);
    }
    int ix0 = ifloor(dmax(0, bx0)), iy0 = ifloor(dmax(0, by0));
    int ix1 = ifloor(dmin(c->w - 1, bx1)), iy1 = ifloor(dmin(c->h - 1, by1));
    double kx = sw / dw, ky = sh / dh;
    int smooth = c->st.smooth && (js_fabs(kx - 1) > 1e-9 || js_fabs(ky - 1) > 1e-9 || m[1] || m[2]);
    const uint8_t *mask = cur_mask(c);
    double ga = c->st.alpha;
    for (int py = iy0; py <= iy1; py++) {
        uint32_t *row = cv->px + (size_t)py * (size_t)c->w;
        for (int px = ix0; px <= ix1; px++) {
            double ux, uy;
            apply(inv, px + 0.5, py + 0.5, &ux, &uy);
            if (ux < dx || uy < dy || ux >= dx + dw || uy >= dy + dh)
                continue;
            double fx = sx + (ux - dx) * kx, fy = sy + (uy - dy) * ky;   /* punkt w obrazku */
            double r, g, b, al;
            if (smooth) {
                double u = fx - 0.5, v = fy - 0.5;
                int iu = ifloor(u), iv = ifloor(v);
                double fu = u - iu, fv = v - iv;
                /* nie wychodzimy poza wybrany kawalek zrodla */
                int lo_x = ifloor(sx), lo_y = ifloor(sy), hi_x = (int)js_ceil(sx + sw) - 1, hi_y = (int)js_ceil(sy + sh) - 1;
                r = g = b = al = 0;
                for (int k = 0; k < 4; k++) {
                    int qx = iu + (k & 1), qy = iv + (k >> 1);
                    if (qx < lo_x) qx = lo_x;
                    if (qy < lo_y) qy = lo_y;
                    if (qx > hi_x) qx = hi_x;
                    if (qy > hi_y) qy = hi_y;
                    uint32_t q = sample(&src, qx, qy);
                    double wgt = (k & 1 ? fu : 1 - fu) * (k >> 1 ? fv : 1 - fv);
                    double qa = (q >> 24) / 255.0 * wgt;
                    r += (q >> 16 & 255) * qa;
                    g += (q >> 8 & 255) * qa;
                    b += (q & 255) * qa;
                    al += qa;
                }
                if (al <= 0)
                    continue;
                r /= al; g /= al; b /= al;
            } else {
                int qx = ifloor(fx), qy = ifloor(fy);
                if (qx < 0 || qy < 0 || qx >= src.w || qy >= src.h)
                    continue;
                uint32_t q = sample(&src, qx, qy);
                al = (q >> 24) / 255.0;
                r = q >> 16 & 255; g = q >> 8 & 255; b = q & 255;
            }
            double k = mask ? mask[(size_t)py * (size_t)c->w + px] / 255.0 : 1;
            if (k > 0 && al > 0)
                blend(&row[px], r, g, b, al * ga, k, c->st.op);
        }
        js_step(rt);
    }
    kfree(copy);
    changed(rt);
    return jv_undef();
}

/* ---------- ImageData (piksele dla skryptu) ---------- */

static jv new_image_data(js_rt *rt, int w, int h, jobj **data)
{
    if (w <= 0 || h <= 0 || (int64_t)w * h > 4 * 1024 * 1024)
        js_throw_error(rt, JS_ERR_RANGE, "Failed to construct 'ImageData': The source width or height is out of range.");
    uint32_t n = (uint32_t)w * (uint32_t)h * 4;
    jobj *a = js_new_array(rt, 0);
    js_array_reserve(rt, a, n);
    for (uint32_t i = 0; i < n; i++)
        a->u.arr.v[i] = jv_num(0);
    a->u.arr.len = n;
    jobj *o = js_new_object(rt, rt->object_proto);
    js_set(rt, jv_obj(o), js_atom(rt, "width"), jv_num(w));
    js_set(rt, jv_obj(o), js_atom(rt, "height"), jv_num(h));
    js_set(rt, jv_obj(o), js_atom(rt, "data"), jv_obj(a));
    js_set(rt, jv_obj(o), js_atom(rt, "colorSpace"), str8(rt, "srgb"));
    *data = a;
    return jv_obj(o);
}

NATIVE(x_create_image_data)
{
    CTX(rt, self);
    jobj *data;
    if (ARG(0).t == JT_OBJ) {                        /* createImageData(innyImageData) */
        int w = (int)js_to_num(rt, js_get(rt, argv[0], js_atom(rt, "width")));
        int h = (int)js_to_num(rt, js_get(rt, argv[0], js_atom(rt, "height")));
        return new_image_data(rt, w, h, &data);
    }
    double a[2];
    if (!nums(rt, argc, argv, 2, a))
        js_throw_error(rt, JS_ERR_TYPE, "Failed to execute 'createImageData': The provided value is non-finite.");
    return new_image_data(rt, (int)js_fabs(a[0]), (int)js_fabs(a[1]), &data);
}

NATIVE(x_get_image_data)
{
    struct c2d *c = CTX(rt, self);
    double a[4];
    if (!nums(rt, argc, argv, 4, a))
        js_throw_error(rt, JS_ERR_TYPE, "Failed to execute 'getImageData': The provided value is non-finite.");
    if (!a[2] || !a[3])
        js_throw_error(rt, JS_ERR_RANGE, "Failed to execute 'getImageData': The source width is 0.");
    if (a[2] < 0) { a[0] += a[2]; a[2] = -a[2]; }
    if (a[3] < 0) { a[1] += a[3]; a[3] = -a[3]; }
    int sx = ifloor(a[0]), sy = ifloor(a[1]), w = (int)a[2], h = (int)a[3];
    jobj *data;
    jv res = new_image_data(rt, w, h, &data);
    struct dom_canvas *cv = pixels(rt, c);
    if (!cv)
        return res;
    jv *v = data->u.arr.v;
    for (int y = 0; y < h; y++) {
        int cy = sy + y;
        if (cy < 0 || cy >= c->h)
            continue;
        for (int x = 0; x < w; x++) {
            int cx = sx + x;
            if (cx < 0 || cx >= c->w)
                continue;
            uint32_t p = cv->px[(size_t)cy * (size_t)c->w + cx];
            jv *o = &v[((size_t)y * (size_t)w + x) * 4];
            o[0] = jv_num(p >> 16 & 255);
            o[1] = jv_num(p >> 8 & 255);
            o[2] = jv_num(p & 255);
            o[3] = jv_num(p >> 24);
        }
    }
    return res;
}

static int byte_of(js_rt *rt, jv v)
{
    double d = v.t == JT_NUM ? v.u.n : js_to_num(rt, v);
    if (!(d > 0))
        return 0;
    if (d >= 255)
        return 255;
    return (int)(d + 0.5);
}

NATIVE(x_put_image_data)
{
    struct c2d *c = CTX(rt, self);
    jv img = ARG(0);
    if (img.t != JT_OBJ)
        js_throw_error(rt, JS_ERR_TYPE, "Failed to execute 'putImageData': parameter 1 is not of type 'ImageData'.");
    double a[2];
    if (!nums(rt, argc - 1 > 0 ? argc - 1 : 0, argv + 1, 2, a))
        return jv_undef();
    int w = (int)js_to_num(rt, js_get(rt, img, js_atom(rt, "width")));
    int h = (int)js_to_num(rt, js_get(rt, img, js_atom(rt, "height")));
    jv data = js_get(rt, img, js_atom(rt, "data"));
    struct dom_canvas *cv = pixels(rt, c);
    if (!cv || w <= 0 || h <= 0 || data.t != JT_OBJ)
        return jv_undef();
    int rx = 0, ry = 0, rw = w, rh = h;              /* "brudny" prostokat (argumenty 4-7) */
    if (argc >= 7) {
        double d[4];
        if (!nums(rt, 4, argv + 3, 4, d))
            return jv_undef();
        rx = (int)d[0]; ry = (int)d[1]; rw = (int)d[2]; rh = (int)d[3];
        if (rw < 0) { rx += rw; rw = -rw; }
        if (rh < 0) { ry += rh; rh = -rh; }
        if (rx < 0) { rw += rx; rx = 0; }
        if (ry < 0) { rh += ry; ry = 0; }
        if (rx + rw > w) rw = w - rx;
        if (ry + rh > h) rh = h - ry;
    }
    int dx = ifloor(a[0]), dy = ifloor(a[1]);
    jobj *arr = data.u.o->cls == JC_ARRAY ? data.u.o : 0;
    for (int y = ry; y < ry + rh; y++) {
        int cy = dy + y;
        if (cy < 0 || cy >= c->h)
            continue;
        for (int x = rx; x < rx + rw; x++) {
            int cx = dx + x;
            if (cx < 0 || cx >= c->w)
                continue;
            uint32_t i = ((uint32_t)y * (uint32_t)w + (uint32_t)x) * 4, comp[4];
            for (int k = 0; k < 4; k++) {
                jv e = arr && i + k < arr->u.arr.len ? arr->u.arr.v[i + k] : arr ? jv_num(0) : js_get_index(rt, data, jv_num(i + k));
                comp[k] = (uint32_t)byte_of(rt, e);
            }
            cv->px[(size_t)cy * (size_t)c->w + cx] = comp[3] ? comp[3] << 24 | comp[0] << 16 | comp[1] << 8 | comp[2] : 0;
        }
        js_step(rt);
    }
    changed(rt);
    return jv_undef();
}

/* ---------- rejestracja ---------- */

void js_canvas_init(js_rt *rt)
{
    jobj *cp = js_new_object(rt, rt->element_proto);
    rt->canvas_proto = cp;
    js_def_fn(rt, cp, "getContext", cv_get_context, 1);
    js_def_getset(rt, cp, "width", cv_width, cv_set_width);
    js_def_getset(rt, cp, "height", cv_height, cv_set_height);
    js_def_fn(rt, cp, "toDataURL", cv_to_data_url, 0);
    js_def_fn(rt, cp, "toBlob", cv_noop, 1);

    jobj *ep = rt->element_proto;                    /* <img>: czy juz pobrany (gry czekaja na obrazki) */
    js_def_getset(rt, ep, "complete", img_complete, 0);
    js_def_getset(rt, ep, "naturalWidth", img_natural_width, 0);
    js_def_getset(rt, ep, "naturalHeight", img_natural_height, 0);

    jobj *x = js_new_object(rt, rt->object_proto);
    rt->ctx2d_proto = x;
    js_def_getset(rt, x, "canvas", x_canvas, 0);
    js_def_getset(rt, x, "fillStyle", x_fill_style, x_set_fill_style);
    js_def_getset(rt, x, "strokeStyle", x_stroke_style, x_set_stroke_style);
    js_def_getset(rt, x, "globalAlpha", x_global_alpha, x_set_global_alpha);
    js_def_getset(rt, x, "lineWidth", x_line_width, x_set_line_width);
    js_def_getset(rt, x, "lineCap", x_line_cap, x_set_line_cap);
    js_def_getset(rt, x, "lineJoin", x_line_join, x_set_line_join);
    js_def_getset(rt, x, "miterLimit", x_miter_limit, x_set_miter_limit);
    js_def_getset(rt, x, "font", x_font, x_set_font);
    js_def_getset(rt, x, "textAlign", x_text_align, x_set_text_align);
    js_def_getset(rt, x, "textBaseline", x_text_baseline, x_set_text_baseline);
    js_def_getset(rt, x, "globalCompositeOperation", x_comp_op, x_set_comp_op);
    js_def_getset(rt, x, "imageSmoothingEnabled", x_smoothing, x_set_smoothing);
    /* cienie, przerywane linie, filtry - przyjmujemy, ale nie rysujemy */
    js_def_val(rt, x, "shadowBlur", jv_num(0));
    js_def_val(rt, x, "shadowColor", str8(rt, "rgba(0, 0, 0, 0)"));
    js_def_val(rt, x, "shadowOffsetX", jv_num(0));
    js_def_val(rt, x, "shadowOffsetY", jv_num(0));
    js_def_val(rt, x, "lineDashOffset", jv_num(0));
    js_def_val(rt, x, "direction", str8(rt, "ltr"));
    js_def_val(rt, x, "filter", str8(rt, "none"));
    js_def_val(rt, x, "imageSmoothingQuality", str8(rt, "low"));
    js_def_fn(rt, x, "save", x_save, 0);
    js_def_fn(rt, x, "restore", x_restore, 0);
    js_def_fn(rt, x, "reset", x_reset, 0);
    js_def_fn(rt, x, "translate", x_translate, 2);
    js_def_fn(rt, x, "scale", x_scale, 2);
    js_def_fn(rt, x, "rotate", x_rotate, 1);
    js_def_fn(rt, x, "transform", x_transform, 6);
    js_def_fn(rt, x, "setTransform", x_set_transform, 6);
    js_def_fn(rt, x, "resetTransform", x_reset_transform, 0);
    js_def_fn(rt, x, "getTransform", x_get_transform, 0);
    js_def_fn(rt, x, "fillRect", x_fill_rect, 4);
    js_def_fn(rt, x, "clearRect", x_clear_rect, 4);
    js_def_fn(rt, x, "strokeRect", x_stroke_rect, 4);
    js_def_fn(rt, x, "beginPath", x_begin_path, 0);
    js_def_fn(rt, x, "closePath", x_close_path, 0);
    js_def_fn(rt, x, "moveTo", x_move_to, 2);
    js_def_fn(rt, x, "lineTo", x_line_to, 2);
    js_def_fn(rt, x, "rect", x_rect, 4);
    js_def_fn(rt, x, "roundRect", x_round_rect, 5);
    js_def_fn(rt, x, "arc", x_arc, 5);
    js_def_fn(rt, x, "arcTo", x_arc_to, 5);
    js_def_fn(rt, x, "ellipse", x_ellipse, 7);
    js_def_fn(rt, x, "quadraticCurveTo", x_quad_to, 4);
    js_def_fn(rt, x, "bezierCurveTo", x_bezier_to, 6);
    js_def_fn(rt, x, "fill", x_fill, 0);
    js_def_fn(rt, x, "stroke", x_stroke, 0);
    js_def_fn(rt, x, "clip", x_clip, 0);
    js_def_fn(rt, x, "isPointInPath", x_is_point_in_path, 2);
    js_def_fn(rt, x, "isPointInStroke", x_false, 2);
    js_def_fn(rt, x, "fillText", x_fill_text, 3);
    js_def_fn(rt, x, "strokeText", x_stroke_text, 3);
    js_def_fn(rt, x, "measureText", x_measure_text, 1);
    js_def_fn(rt, x, "drawImage", x_draw_image, 3);
    js_def_fn(rt, x, "createImageData", x_create_image_data, 2);
    js_def_fn(rt, x, "getImageData", x_get_image_data, 4);
    js_def_fn(rt, x, "putImageData", x_put_image_data, 3);
    js_def_fn(rt, x, "createLinearGradient", x_linear_gradient, 4);
    js_def_fn(rt, x, "createRadialGradient", x_radial_gradient, 6);
    js_def_fn(rt, x, "createPattern", x_create_pattern, 2);
    js_def_fn(rt, x, "setLineDash", x_noop, 1);
    js_def_fn(rt, x, "getLineDash", x_get_line_dash, 0);

    jobj *gp = js_new_object(rt, rt->object_proto);
    js_def_fn(rt, gp, "addColorStop", g_add_color_stop, 2);
    js_define(rt, x, js_atom(rt, "__gradproto"), jv_obj(gp), 0);
}

/* ---------- API dla C: rysowanie SVG (js_svg.c) bez silnika JS (rt = 0) ---------- */

struct c2d *c2d_new(int w, int h)
{
    struct c2d *c = kzalloc(sizeof(*c));
    if (!c)
        return 0;
    reset_state(&c->st);
    c->w = w;
    c->h = h;
    c->cov = kzalloc(sizeof(float) * (size_t)(w + 2));
    if (!c->cov) {
        kfree(c);
        return 0;
    }
    return c;
}

void c2d_free(struct c2d *c)
{
    if (!c)
        return;
    kfree(c->pts);
    kfree(c->subs);
    kfree(c->e);
    kfree(c->order);
    kfree(c->tmp);
    kfree(c->cov);
    kfree(c);
}

void c2d_begin(struct c2d *c) { c->npts = c->nsubs = 0; }
void c2d_matrix(struct c2d *c, const double *m) { memcpy(c->st.m, m, sizeof(c->st.m)); }
void c2d_move(struct c2d *c, double x, double y) { user_move(0, c, x, y); }
void c2d_line(struct c2d *c, double x, double y) { user_line(0, c, x, y); }
void c2d_close(struct c2d *c) { close_path(0, c); }
void c2d_rect(struct c2d *c, double x, double y, double w, double h) { add_rect_path(0, c, x, y, w, h); }

void c2d_ellipse(struct c2d *c, double cx, double cy, double rx, double ry)
{
    add_ellipse(0, c, cx, cy, rx, ry, 0, 0, 2 * PI, 0);
    close_path(0, c);
}

/* Kolor CSS -> r, g, b (0..255), a (0..1). 0 = nie kolor */
int c2d_color(const char *s, double *rgba) { return parse_color(s, rgba); }

static void c_paint(struct paint *p, const double *rgba, double opacity)
{
    memset(p, 0, sizeof(*p));
    p->type = PT_COLOR;
    p->r = rgba[0];
    p->g = rgba[1];
    p->b = rgba[2];
    p->a = rgba[3] * opacity;
}

void c2d_fill(struct c2d *c, struct dom_canvas *cv, const double *rgba, double opacity, int evenodd)
{
    struct paint p;
    c_paint(&p, rgba, opacity);
    c->ne = 0;
    path_edges(0, c);
    fill_edges(0, c, cv, &p, evenodd, CO_OVER);
}

void c2d_stroke(struct c2d *c, struct dom_canvas *cv, const double *rgba, double opacity, double width,
                int cap, int join)
{
    c_paint(&c->st.stroke, rgba, opacity);
    c->st.line_width = width;
    c->st.cap = cap;
    c->st.join = join;
    stroke_path(0, c, cv);
}

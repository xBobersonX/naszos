/* <svg> w stronie - grafika wektorowa.
 *
 * Parser HTML zostawia <svg> w drzewie DOM razem z dziecmi (<rect>, <circle>,
 * <g>...). Uklad strony traktuje go jak obrazek (element "zastepowany"),
 * a przy rysowaniu ten plik przechodzi po dzieciach i maluje ksztalty na
 * wlasnym plotnie elementu (n->canvas) rasteryzatorem z <canvas>.
 *
 * viewBox="x y w h" mowi, jaki kawalek ukladu wspolrzednych SVG pokazac -
 * skalujemy go do rozmiaru elementu (domyslnie z zachowaniem proporcji,
 * na srodku). Nazwa pliku zaczyna sie od js_, bo liczy na double (SSE).
 *
 * Na razie: rect, circle, ellipse, line, polyline, polygon, g; atrybuty
 * fill, stroke, stroke-width, opacity, fill-opacity, stroke-opacity,
 * fill-rule, stroke-linecap, stroke-linejoin, currentColor. */

#include "svg.h"
#include "js_int.h"
#include "dom.h"
#include "css.h"
#include "heap.h"
#include "string.h"
#include "http.h"

#define PI 3.14159265358979323846

struct sv {
    struct c2d *c;
    struct dom_canvas *cv;
    double m[6];                     /* viewBox -> piksele plotna */
    double vw, vh;                   /* rozmiar viewBox (do procentow) */
    int count;                       /* zabezpieczenie przed ogromnymi rysunkami */
};

/* Ustawienia dziedziczone przez dzieci (jak w CSS) */
struct pa {
    const char *fill, *stroke;
    double sw, opacity, fop, sop;
    int evenodd, cap, join;
    uint32_t cur;                    /* currentColor */
};

static int is_ws(char c) { return c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r'; }
static int is_dig(char c) { return c >= '0' && c <= '9'; }

static int name_is(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a;
        if (x != *b)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

/* Liczba SVG (1, -2.5, .5, 1e3). Zwraca koniec albo 0, gdy nie ma liczby */
static const char *num(const char *s, double *out)
{
    while (is_ws(*s))
        s++;
    double sign = 1, v = 0;
    int got = 0;
    if (*s == '+' || *s == '-')
        sign = *s++ == '-' ? -1 : 1;
    for (; is_dig(*s); s++, got = 1)
        v = v * 10 + (*s - '0');
    if (*s == '.') {
        double f = 0.1;
        for (s++; is_dig(*s); s++, f *= 0.1, got = 1)
            v += (*s - '0') * f;
    }
    if (!got)
        return 0;
    if ((*s == 'e' || *s == 'E') && (is_dig(s[1]) || ((s[1] == '-' || s[1] == '+') && is_dig(s[2])))) {
        int es = s[1] == '-' ? -1 : 1, e = 0;
        s += s[1] == '-' || s[1] == '+' ? 2 : 1;
        for (; is_dig(*s); s++)
            if (e < 400)
                e = e * 10 + (*s - '0');
        while (e-- > 0)
            v = es > 0 ? v * 10 : v / 10;
    }
    *out = sign * v;
    return s;
}

/* Atrybut-dlugosc: liczba, "10px" albo "50%" (procent od ref) */
static double anum(const struct dom_node *n, const char *name, double def, double ref)
{
    const char *v = dom_attr(n, name);
    double x;
    const char *e = v ? num(v, &x) : 0;
    if (!e)
        return def;
    return *e == '%' ? x * ref / 100 : x;
}

static int viewbox(const struct dom_node *n, double *vb)
{
    const char *s = dom_attr(n, "viewbox");
    for (int i = 0; i < 4; i++)
        if (!s || !(s = num(s, &vb[i])))
            return 0;
    return vb[2] > 0 && vb[3] > 0;
}

void svg_size(const struct dom_node *n, int *w, int *h)
{
    double vb[4];
    if (viewbox(n, vb)) {
        *w = (int)(vb[2] + 0.5);
        *h = (int)(vb[3] + 0.5);
    } else {
        *w = 300;
        *h = 150;
    }
}

/* Kolor z atrybutu. 0 = nie malujemy (none, brak, nieznany) */
static int paint_color(const char *v, const struct pa *pa, double *rgba)
{
    if (!v)
        return 0;
    while (is_ws(*v))
        v++;
    if (name_is(v, "none") || name_is(v, "transparent"))
        return 0;
    if (name_is(v, "currentcolor")) {
        rgba[0] = (pa->cur >> 16) & 255;
        rgba[1] = (pa->cur >> 8) & 255;
        rgba[2] = pa->cur & 255;
        rgba[3] = 1;
        return 1;
    }
    if (str_starts_ci(v, "url(")) {                      /* gradient (jeszcze nie) - kolor zapasowy po ")" */
        while (*v && *v != ')')
            v++;
        if (!*v)
            return 0;
        return paint_color(v + 1, pa, rgba);
    }
    return c2d_color(v, rgba);
}

static double unit(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

static void inherit(const struct dom_node *n, struct pa *pa)
{
    const char *v;
    if (n->style)
        pa->cur = n->style->color;
    if ((v = dom_attr(n, "fill")))
        pa->fill = v;
    if ((v = dom_attr(n, "stroke")))
        pa->stroke = v;
    pa->sw = anum(n, "stroke-width", pa->sw, 100);
    pa->opacity *= unit(anum(n, "opacity", 1, 1));  /* opacity nie dziedziczy sie, ale mnozy (w przyblizeniu) */
    pa->fop = unit(anum(n, "fill-opacity", pa->fop, 1));
    pa->sop = unit(anum(n, "stroke-opacity", pa->sop, 1));
    if ((v = dom_attr(n, "fill-rule")))
        pa->evenodd = name_is(v, "evenodd");
    if ((v = dom_attr(n, "stroke-linecap")))
        pa->cap = name_is(v, "round") ? 1 : name_is(v, "square") ? 2 : 0;
    if ((v = dom_attr(n, "stroke-linejoin")))
        pa->join = name_is(v, "round") ? 1 : name_is(v, "bevel") ? 2 : 0;
}

/* Prostokat z zaokraglonymi rogami (rx, ry) */
static void round_rect(struct c2d *c, double x, double y, double w, double h, double rx, double ry)
{
    static const double cx[4] = { 1, 1, 0, 0 }, cy[4] = { 0, 1, 1, 0 };
    for (int k = 0; k < 4; k++) {                        /* rogi: prawy gorny, prawy dolny, lewy dolny, lewy gorny */
        double ox = x + (cx[k] ? w - rx : rx), oy = y + (cy[k] ? h - ry : ry);
        double a0 = -PI / 2 + k * PI / 2;
        for (int i = 0; i <= 8; i++) {
            double a = a0 + PI / 2 * i / 8;
            double px = ox + rx * js_cos(a), py = oy + ry * js_sin(a);
            if (k == 0 && i == 0)
                c2d_move(c, px, py);
            else
                c2d_line(c, px, py);
        }
    }
    c2d_close(c);
}

/* Lista punktow "x,y x,y ..." (polyline, polygon) */
static int points(struct c2d *c, const char *s)
{
    double x, y;
    int n = 0;
    while (s && (s = num(s, &x)) && (s = num(s, &y))) {
        if (n++)
            c2d_line(c, x, y);
        else
            c2d_move(c, x, y);
    }
    return n;
}

static void shape(struct sv *S, const struct dom_node *n, const struct pa *pa)
{
    struct c2d *c = S->c;
    const char *name = n->name;
    int fillable = 1;
    c2d_begin(c);
    c2d_matrix(c, S->m);
    if (name_is(name, "rect")) {
        double x = anum(n, "x", 0, S->vw), y = anum(n, "y", 0, S->vh);
        double w = anum(n, "width", 0, S->vw), h = anum(n, "height", 0, S->vh);
        if (w <= 0 || h <= 0)
            return;
        double rx = anum(n, "rx", -1, S->vw), ry = anum(n, "ry", -1, S->vh);
        if (rx < 0) rx = ry;
        if (ry < 0) ry = rx;
        if (rx > w / 2) rx = w / 2;
        if (ry > h / 2) ry = h / 2;
        if (rx > 0 && ry > 0)
            round_rect(c, x, y, w, h, rx, ry);
        else
            c2d_rect(c, x, y, w, h);
    } else if (name_is(name, "circle") || name_is(name, "ellipse")) {
        double cx = anum(n, "cx", 0, S->vw), cy = anum(n, "cy", 0, S->vh), rx, ry;
        if (name[0] == 'c' || name[0] == 'C')
            rx = ry = anum(n, "r", 0, js_sqrt((S->vw * S->vw + S->vh * S->vh) / 2));
        else {
            rx = anum(n, "rx", 0, S->vw);
            ry = anum(n, "ry", 0, S->vh);
        }
        if (rx <= 0 || ry <= 0)
            return;
        c2d_ellipse(c, cx, cy, rx, ry);
    } else if (name_is(name, "line")) {
        c2d_move(c, anum(n, "x1", 0, S->vw), anum(n, "y1", 0, S->vh));
        c2d_line(c, anum(n, "x2", 0, S->vw), anum(n, "y2", 0, S->vh));
        fillable = 0;
    } else if (name_is(name, "polyline") || name_is(name, "polygon")) {
        if (points(c, dom_attr(n, "points")) < 2)
            return;
        if (name[4] == 'g' || name[4] == 'G')
            c2d_close(c);
    } else {
        return;
    }
    double rgba[4];
    if (fillable && paint_color(pa->fill, pa, rgba))
        c2d_fill(c, S->cv, rgba, pa->opacity * pa->fop, pa->evenodd);
    if (pa->sw > 0 && paint_color(pa->stroke, pa, rgba))
        c2d_stroke(c, S->cv, rgba, pa->opacity * pa->sop, pa->sw, pa->cap, pa->join);
}

static void walk(struct sv *S, const struct dom_node *n, struct pa pa, int depth)
{
    for (const struct dom_node *k = n->first; k; k = k->next) {
        if (k->type != N_ELEMENT || ++S->count > 20000 || depth > 64)
            continue;
        if (k->style && k->style->display == D_NONE)
            continue;
        const char *d = dom_attr(k, "display");
        if (d && name_is(d, "none"))
            continue;
        struct pa p = pa;
        inherit(k, &p);
        if (name_is(k->name, "g") || name_is(k->name, "a") || name_is(k->name, "switch") || name_is(k->name, "svg"))
            walk(S, k, p, depth + 1);
        else
            shape(S, k, &p);
    }
}

void svg_paint(struct dom_doc *d, struct dom_node *n, int w, int h)
{
    if (w <= 0 || h <= 0 || (int64_t)w * h > CANVAS_MAX_PIXELS)
        return;
    struct dom_canvas *cv = n->canvas;
    if (cv && cv->w == w && cv->h == h)
        return;                                          /* juz narysowany w tym rozmiarze */
    if (!cv) {
        cv = kzalloc(sizeof(*cv));
        if (!cv)
            return;
        cv->next = d->canvases;
        d->canvases = cv;
        n->canvas = cv;
    }
    kfree(cv->px);
    cv->px = kzalloc((size_t)w * (size_t)h * 4);
    cv->w = cv->px ? w : 0;
    cv->h = cv->px ? h : 0;
    if (!cv->px)
        return;
    struct sv S;
    memset(&S, 0, sizeof(S));
    S.c = c2d_new(w, h);
    if (!S.c)
        return;
    S.cv = cv;
    S.m[0] = S.m[3] = 1;
    S.vw = w;
    S.vh = h;
    double vb[4];
    if (viewbox(n, vb)) {
        double sx = w / vb[2], sy = h / vb[3];
        const char *par = dom_attr(n, "preserveaspectratio");
        if (!par || !str_starts_ci(par, "none"))
            sx = sy = sx < sy ? sx : sy;                 /* "meet": caly rysunek, na srodku */
        S.m[0] = sx;
        S.m[3] = sy;
        S.m[4] = (w - vb[2] * sx) / 2 - vb[0] * sx;
        S.m[5] = (h - vb[3] * sy) / 2 - vb[1] * sy;
        S.vw = vb[2];
        S.vh = vb[3];
    }
    struct pa pa = { "black", 0, 1, 1, 1, 1, 0, 0, 0, 0 };
    inherit(n, &pa);
    walk(&S, n, pa, 0);
    c2d_free(S.c);
}

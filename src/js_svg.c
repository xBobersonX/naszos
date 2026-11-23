/* <svg> w stronie - grafika wektorowa.
 *
 * Parser HTML zostawia <svg> w drzewie DOM razem z dziecmi (<path>, <rect>,
 * <g>...). Uklad strony traktuje go jak obrazek (element "zastepowany"),
 * a przy rysowaniu ten plik przechodzi po dzieciach i maluje ksztalty na
 * wlasnym plotnie elementu (n->canvas) rasteryzatorem z <canvas>.
 * <img src="x.svg"> dziala tak samo: plik parsujemy na drzewo DOM i rysujemy.
 *
 * viewBox="x y w h" mowi, jaki kawalek ukladu wspolrzednych SVG pokazac -
 * skalujemy go do rozmiaru elementu (domyslnie z zachowaniem proporcji,
 * na srodku). transform="..." to macierz 2x3, mnozona w dol drzewa.
 * Krzywe (Bezier, luki) zamieniamy na krotkie odcinki.
 *
 * Kolory: atrybut fill="..." przegrywa z CSS (fill: ... w regule albo
 * w style="..."), a oba sa dziedziczone przez dzieci. Gradient (url(#id))
 * malujemy na razie kolorem jego pierwszego <stop>.
 *
 * Nazwa pliku zaczyna sie od js_, bo liczy na double (SSE). */

#include "svg.h"
#include "js_int.h"
#include "dom.h"
#include "css.h"
#include "heap.h"
#include "string.h"
#include "http.h"
#include "image.h"

#define PI 3.14159265358979323846

struct sv {
    struct c2d *c;
    struct dom_canvas *cv;
    const struct dom_node *root;
    double vw, vh;                   /* rozmiar viewBox (do procentow) */
    int count;                       /* zabezpieczenie przed ogromnymi rysunkami */
};

enum { PK_NONE, PK_COLOR, PK_CURRENT };
struct pv { int kind; double c[4]; };

/* Ustawienia dziedziczone przez dzieci (jak w CSS) */
struct pa {
    struct pv fill, stroke;
    double sw, opacity, fop, sop;
    int evenodd, cap, join;
    uint32_t cur;                    /* currentColor */
    double m[6];                     /* wspolrzedne elementu -> piksele plotna */
};

static int is_ws(char c) { return c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r'; }
static int is_dig(char c) { return c >= '0' && c <= '9'; }
static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

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

/* ---------- macierze ---------- */

static void mul(double *m, const double *t)              /* m = m * t (t dziala pierwsze) */
{
    double r[6] = {
        m[0] * t[0] + m[2] * t[1], m[1] * t[0] + m[3] * t[1],
        m[0] * t[2] + m[2] * t[3], m[1] * t[2] + m[3] * t[3],
        m[0] * t[4] + m[2] * t[5] + m[4], m[1] * t[4] + m[3] * t[5] + m[5],
    };
    memcpy(m, r, sizeof(r));
}

/* transform="translate(10 20) rotate(45) scale(2) matrix(...) skewX(..)" */
static void transform(const char *s, double *m)
{
    while (s && *s) {
        while (is_ws(*s))
            s++;
        const char *name = s;
        while (is_alpha(*s))
            s++;
        unsigned nl = (unsigned)(s - name);
        while (*s && *s != '(')
            s++;
        if (!*s || !nl)
            return;
        s++;
        double a[6];
        int n = 0;
        const char *e;
        while (n < 6 && (e = num(s, &a[n]))) {
            s = e;
            n++;
        }
        while (*s && *s != ')')
            s++;
        if (*s)
            s++;
        double t[6] = { 1, 0, 0, 1, 0, 0 };
        if (nl == 6 && memcmp(name, "matrix", 6) == 0 && n == 6)
            memcpy(t, a, sizeof(t));
        else if (nl == 9 && memcmp(name, "translate", 9) == 0 && n >= 1) {
            t[4] = a[0];
            t[5] = n > 1 ? a[1] : 0;
        } else if (nl == 5 && memcmp(name, "scale", 5) == 0 && n >= 1) {
            t[0] = a[0];
            t[3] = n > 1 ? a[1] : a[0];
        } else if (nl == 6 && memcmp(name, "rotate", 6) == 0 && n >= 1) {
            double r = a[0] * PI / 180, c = js_cos(r), sn = js_sin(r);
            double cx = n >= 3 ? a[1] : 0, cy = n >= 3 ? a[2] : 0;
            t[0] = c; t[1] = sn; t[2] = -sn; t[3] = c;
            t[4] = cx - c * cx + sn * cy;
            t[5] = cy - sn * cx - c * cy;
        } else if (nl == 5 && memcmp(name, "skewX", 5) == 0 && n >= 1)
            t[2] = js_tan(a[0] * PI / 180);
        else if (nl == 5 && memcmp(name, "skewY", 5) == 0 && n >= 1)
            t[1] = js_tan(a[0] * PI / 180);
        else
            continue;
        mul(m, t);
    }
}

/* ---------- kolory ---------- */

static const struct dom_node *find_id(const struct dom_node *n, const char *id, unsigned len, int depth)
{
    for (const struct dom_node *k = n->first; k; k = k->next) {
        if (k->type != N_ELEMENT)
            continue;
        const char *v = dom_attr(k, "id");
        if (v && strlen(v) == len && memcmp(v, id, len) == 0)
            return k;
        const struct dom_node *f = depth < 32 ? find_id(k, id, len, depth + 1) : 0;
        if (f)
            return f;
    }
    return 0;
}

static int parse_paint(const struct sv *S, const char *v, struct pv *out, int depth);

/* Gradient url(#id): kolor jego pierwszego <stop> (prawdziwe gradienty - kiedys) */
static int grad_color(const struct sv *S, const char *v, struct pv *out, int depth)
{
    while (*v && *v != '#')
        v++;
    if (!*v || !S || depth > 2)
        return 0;
    const char *id = ++v;
    while (*v && *v != ')' && *v != '"' && *v != '\'')
        v++;
    const struct dom_node *g = find_id(S->root, id, (unsigned)(v - id), 0);
    if (!g)
        return 0;
    for (const struct dom_node *k = g->first; k; k = k->next)
        if (k->type == N_ELEMENT && name_is(k->name, "stop")) {
            const char *sc = dom_attr(k, "stop-color");
            const char *st = dom_attr(k, "style");
            if (st && (st = strstr_ci(st, "stop-color:")))
                sc = st + 11;
            return parse_paint(S, sc ? sc : "black", out, depth + 1);
        }
    const char *href = dom_attr(g, "href");                  /* gradient dziedziczacy stopy */
    if (!href)
        href = dom_attr(g, "xlink:href");
    return href ? grad_color(S, href, out, depth + 1) : 0;
}

/* Wartosc fill/stroke. 0 = nieznana (zostaje poprzednia) */
static int parse_paint(const struct sv *S, const char *v, struct pv *out, int depth)
{
    while (is_ws(*v))
        v++;
    if (name_is(v, "none") || name_is(v, "transparent")) {
        out->kind = PK_NONE;
        return 1;
    }
    if (str_starts_ci(v, "currentcolor")) {
        out->kind = PK_CURRENT;
        return 1;
    }
    if (str_starts_ci(v, "url(")) {
        if (grad_color(S, v, out, depth))
            return 1;
        while (*v && *v != ')')                          /* kolor zapasowy po ")" */
            v++;
        if (*v && parse_paint(S, v + 1, out, depth))
            return 1;
        out->kind = PK_NONE;
        return 1;
    }
    char buf[64];                                        /* do ";" (style="stop-color:red;...") */
    unsigned l = 0;
    while (v[l] && v[l] != ';' && l + 1 < sizeof(buf)) {
        buf[l] = v[l];
        l++;
    }
    buf[l] = 0;
    if (!c2d_color(buf, out->c))
        return 0;
    out->kind = PK_COLOR;
    return 1;
}

static double unit(double x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

static void css_paint(struct pv *p, int kind, uint32_t c, int a)
{
    p->kind = kind == 1 ? PK_NONE : kind == 2 ? PK_CURRENT : PK_COLOR;
    p->c[0] = (c >> 16) & 255;
    p->c[1] = (c >> 8) & 255;
    p->c[2] = c & 255;
    p->c[3] = a / 255.0;
}

static void inherit(const struct sv *S, const struct dom_node *n, struct pa *pa)
{
    const char *v;
    const struct style *st = n->style;
    if (st)
        pa->cur = st->color;
    if ((v = dom_attr(n, "fill")))
        parse_paint(S, v, &pa->fill, 0);
    if ((v = dom_attr(n, "stroke")))
        parse_paint(S, v, &pa->stroke, 0);
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
    if (st && (st->svg_set & 1))                         /* CSS wygrywa z atrybutem */
        css_paint(&pa->fill, st->fill_kind, st->fill, st->fill_a);
    if (st && (st->svg_set & 2))
        css_paint(&pa->stroke, st->stroke_kind, st->stroke, st->stroke_a);
    if (st && (st->svg_set & 4))
        pa->sw = st->stroke_w16 / 16.0;
    if ((v = dom_attr(n, "transform")))
        transform(v, pa->m);
}

/* ---------- ksztalty ---------- */

static int segs(double px)
{
    int n = (int)(px / 3) + 2;
    return n > 100 ? 100 : n;
}

static double dist(double ax, double ay, double bx, double by)
{
    return js_sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
}

static void cubic(struct c2d *c, double sc, double x0, double y0, double x1, double y1, double x2, double y2,
                  double x3, double y3)
{
    int n = segs((dist(x0, y0, x1, y1) + dist(x1, y1, x2, y2) + dist(x2, y2, x3, y3)) * sc);
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        double a = u * u * u, b = 3 * u * u * t, d = 3 * u * t * t, e = t * t * t;
        c2d_line(c, a * x0 + b * x1 + d * x2 + e * x3, a * y0 + b * y1 + d * y2 + e * y3);
    }
}

static void quad(struct c2d *c, double sc, double x0, double y0, double x1, double y1, double x2, double y2)
{
    int n = segs((dist(x0, y0, x1, y1) + dist(x1, y1, x2, y2)) * sc);
    for (int i = 1; i <= n; i++) {
        double t = (double)i / n, u = 1 - t;
        c2d_line(c, u * u * x0 + 2 * u * t * x1 + t * t * x2, u * u * y0 + 2 * u * t * y1 + t * t * y2);
    }
}

static double angle(double ux, double uy, double vx, double vy)
{
    return js_atan2(ux * vy - uy * vx, ux * vx + uy * vy);
}

/* Luk eliptyczny (komenda A) - z punktow koncowych na srodek i katy (jak w specyfikacji SVG) */
static void arc(struct c2d *c, double sc, double x1, double y1, double rx, double ry, double deg, int fa, int fs,
                double x2, double y2)
{
    if (x1 == x2 && y1 == y2)
        return;
    rx = js_fabs(rx);
    ry = js_fabs(ry);
    if (rx == 0 || ry == 0) {
        c2d_line(c, x2, y2);
        return;
    }
    double phi = deg * PI / 180, cp = js_cos(phi), sp = js_sin(phi);
    double dx = (x1 - x2) / 2, dy = (y1 - y2) / 2;
    double xp = cp * dx + sp * dy, yp = -sp * dx + cp * dy;
    double lam = xp * xp / (rx * rx) + yp * yp / (ry * ry);
    if (lam > 1) {
        double s = js_sqrt(lam);
        rx *= s;
        ry *= s;
    }
    double nu = rx * rx * ry * ry - rx * rx * yp * yp - ry * ry * xp * xp;
    double de = rx * rx * yp * yp + ry * ry * xp * xp;
    double co = de > 0 && nu > 0 ? js_sqrt(nu / de) : 0;
    if (fa == fs)
        co = -co;
    double cxp = co * rx * yp / ry, cyp = -co * ry * xp / rx;
    double cx = cp * cxp - sp * cyp + (x1 + x2) / 2, cy = sp * cxp + cp * cyp + (y1 + y2) / 2;
    double ux = (xp - cxp) / rx, uy = (yp - cyp) / ry, vx = (-xp - cxp) / rx, vy = (-yp - cyp) / ry;
    double th = angle(1, 0, ux, uy), dth = angle(ux, uy, vx, vy);
    if (!fs && dth > 0)
        dth -= 2 * PI;
    else if (fs && dth < 0)
        dth += 2 * PI;
    int n = segs(js_fabs(dth) * (rx > ry ? rx : ry) * sc);
    for (int i = 1; i <= n; i++) {
        double t = th + dth * i / n, ex = rx * js_cos(t), ey = ry * js_sin(t);
        c2d_line(c, cx + ex * cp - ey * sp, cy + ex * sp + ey * cp);
    }
}

/* <path d="M10 10 L20 20 C... Z"> */
static void path_d(struct c2d *c, const char *s, double sc)
{
    double cx = 0, cy = 0, sx = 0, sy = 0, lx = 0, ly = 0;   /* biezacy, poczatek, ostatni punkt kontrolny */
    char cmd = 0, prev = 0;
    while (*s) {
        while (is_ws(*s))
            s++;
        if (!*s)
            break;
        if (is_alpha(*s)) {
            cmd = *s++;
            if (cmd == 'z' || cmd == 'Z') {
                c2d_close(c);
                cx = sx;
                cy = sy;
                prev = 'Z';
                continue;
            }
        } else if (!cmd || prev == 'Z') {
            return;
        }
        int rel = cmd >= 'a';
        char C = rel ? (char)(cmd - 32) : cmd;
        int need = C == 'M' || C == 'L' || C == 'T' ? 2 : C == 'H' || C == 'V' ? 1 : C == 'C' ? 6 :
                   C == 'S' || C == 'Q' ? 4 : C == 'A' ? 7 : 0;
        if (!need)
            return;
        double a[7];
        for (int i = 0; i < need; i++) {
            if (C == 'A' && (i == 3 || i == 4)) {        /* flagi luku: pojedyncze 0/1, bez odstepu */
                while (is_ws(*s))
                    s++;
                if (*s != '0' && *s != '1')
                    return;
                a[i] = *s++ - '0';
            } else if (!(s = num(s, &a[i]))) {
                return;
            }
        }
        double ox = rel ? cx : 0, oy = rel ? cy : 0;
        switch (C) {
        case 'M':
            cx = sx = a[0] + ox;
            cy = sy = a[1] + oy;
            c2d_move(c, cx, cy);
            cmd = rel ? 'l' : 'L';                        /* kolejne pary to linie */
            break;
        case 'L':
            cx = a[0] + ox;
            cy = a[1] + oy;
            c2d_line(c, cx, cy);
            break;
        case 'H':
            cx = a[0] + ox;
            c2d_line(c, cx, cy);
            break;
        case 'V':
            cy = a[0] + oy;
            c2d_line(c, cx, cy);
            break;
        case 'C':
        case 'S': {
            double x1, y1, k = C == 'C' ? 2 : 0;
            if (C == 'C') {
                x1 = a[0] + ox;
                y1 = a[1] + oy;
            } else if (prev == 'C' || prev == 'S') {
                x1 = 2 * cx - lx;
                y1 = 2 * cy - ly;
            } else {
                x1 = cx;
                y1 = cy;
            }
            int i = (int)k;
            lx = a[i] + ox;
            ly = a[i + 1] + oy;
            double x = a[i + 2] + ox, y = a[i + 3] + oy;
            cubic(c, sc, cx, cy, x1, y1, lx, ly, x, y);
            cx = x;
            cy = y;
            break;
        }
        case 'Q':
        case 'T': {
            double x, y;
            if (C == 'Q') {
                lx = a[0] + ox;
                ly = a[1] + oy;
                x = a[2] + ox;
                y = a[3] + oy;
            } else {
                if (prev == 'Q' || prev == 'T') {
                    lx = 2 * cx - lx;
                    ly = 2 * cy - ly;
                } else {
                    lx = cx;
                    ly = cy;
                }
                x = a[0] + ox;
                y = a[1] + oy;
            }
            quad(c, sc, cx, cy, lx, ly, x, y);
            cx = x;
            cy = y;
            break;
        }
        case 'A': {
            double x = a[5] + ox, y = a[6] + oy;
            arc(c, sc, cx, cy, a[0], a[1], a[2], (int)a[3], (int)a[4], x, y);
            cx = x;
            cy = y;
            break;
        }
        }
        prev = C;
    }
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

static int paint_rgba(const struct pv *p, const struct pa *pa, double *rgba)
{
    if (p->kind == PK_NONE)
        return 0;
    if (p->kind == PK_CURRENT) {
        rgba[0] = (pa->cur >> 16) & 255;
        rgba[1] = (pa->cur >> 8) & 255;
        rgba[2] = pa->cur & 255;
        rgba[3] = 1;
    } else {
        memcpy(rgba, p->c, sizeof(p->c));
    }
    return 1;
}

static void shape(struct sv *S, const struct dom_node *n, const struct pa *pa)
{
    struct c2d *c = S->c;
    const char *name = n->name;
    int fillable = 1;
    const double *m = pa->m;
    double sc = js_sqrt(js_fabs(m[0] * m[3] - m[1] * m[2]));
    c2d_begin(c);
    c2d_matrix(c, m);
    if (name_is(name, "path")) {
        const char *d = dom_attr(n, "d");
        if (!d)
            return;
        path_d(c, d, sc);
    } else if (name_is(name, "rect")) {
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
    if (fillable && paint_rgba(&pa->fill, pa, rgba))
        c2d_fill(c, S->cv, rgba, pa->opacity * pa->fop, pa->evenodd);
    if (pa->sw > 0 && paint_rgba(&pa->stroke, pa, rgba))
        c2d_stroke(c, S->cv, rgba, pa->opacity * pa->sop, pa->sw, pa->cap, pa->join);
}

static void walk(struct sv *S, const struct dom_node *n, const struct pa *pa, int depth);

static void element(struct sv *S, const struct dom_node *k, const struct pa *pa, int depth)
{
    if (k->type != N_ELEMENT || ++S->count > 20000 || depth > 64)
        return;
    if (k->style && k->style->display == D_NONE)
        return;
    const char *d = dom_attr(k, "display");
    if (d && name_is(d, "none"))
        return;
    struct pa p = *pa;
    inherit(S, k, &p);
    if (name_is(k->name, "g") || name_is(k->name, "a") || name_is(k->name, "switch") || name_is(k->name, "svg")) {
        walk(S, k, &p, depth + 1);
    } else if (name_is(k->name, "use")) {                /* <use href="#id" x y> - kopia innego elementu */
        const char *h = dom_attr(k, "href");
        if (!h)
            h = dom_attr(k, "xlink:href");
        if (!h || *h != '#')
            return;
        const struct dom_node *t = find_id(S->root, h + 1, (unsigned)strlen(h + 1), 0);
        if (!t || t == k)
            return;
        double tr[6] = { 1, 0, 0, 1, anum(k, "x", 0, S->vw), anum(k, "y", 0, S->vh) };
        mul(p.m, tr);
        if (name_is(t->name, "symbol"))
            walk(S, t, &p, depth + 1);
        else
            element(S, t, &p, depth + 1);
    } else {
        shape(S, k, &p);
    }
}

static void walk(struct sv *S, const struct dom_node *n, const struct pa *pa, int depth)
{
    for (const struct dom_node *k = n->first; k; k = k->next)
        element(S, k, pa, depth);
}

/* Rysuje drzewo <svg> na plotnie cv (w x h) */
static void draw(struct dom_node *n, struct dom_canvas *cv, int w, int h)
{
    struct sv S;
    memset(&S, 0, sizeof(S));
    S.c = c2d_new(w, h);
    if (!S.c)
        return;
    S.cv = cv;
    S.root = n;
    S.vw = w;
    S.vh = h;
    struct pa pa;
    memset(&pa, 0, sizeof(pa));
    pa.fill.kind = PK_COLOR;
    pa.fill.c[3] = 1;                                    /* domyslnie czarne wypelnienie, bez obrysu */
    pa.sw = pa.opacity = pa.fop = pa.sop = 1;
    pa.m[0] = pa.m[3] = 1;
    double vb[4];
    if (viewbox(n, vb)) {
        double sx = w / vb[2], sy = h / vb[3];
        const char *par = dom_attr(n, "preserveaspectratio");
        if (!par || !str_starts_ci(par, "none"))
            sx = sy = sx < sy ? sx : sy;                 /* "meet": caly rysunek, na srodku */
        pa.m[0] = sx;
        pa.m[3] = sy;
        pa.m[4] = (w - vb[2] * sx) / 2 - vb[0] * sx;
        pa.m[5] = (h - vb[3] * sy) / 2 - vb[1] * sy;
        S.vw = vb[2];
        S.vh = vb[3];
    }
    inherit(&S, n, &pa);
    walk(&S, n, &pa, 0);
    c2d_free(S.c);
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
    if (cv->px)
        draw(n, cv, w, h);
}

/* ---------- <img src="x.svg"> ---------- */

static struct dom_node *find_svg(struct dom_node *n, int depth)
{
    for (struct dom_node *k = n->first; k; k = k->next) {
        if (k->type != N_ELEMENT)
            continue;
        if (k->tag == T_SVG)
            return k;
        struct dom_node *f = depth < 8 ? find_svg(k, depth + 1) : 0;
        if (f)
            return f;
    }
    return 0;
}

int svg_is(const uint8_t *data, unsigned len)
{
    unsigned i = 0;
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB)
        i = 3;
    while (i < len && (data[i] == ' ' || data[i] == '\n' || data[i] == '\r' || data[i] == '\t'))
        i++;
    if (i >= len || data[i] != '<')
        return 0;
    for (unsigned k = i; k + 4 <= len && k < i + 4096; k++)
        if (memcmp(data + k, "<svg", 4) == 0)
            return 1;
    return 0;
}

int svg_image(const uint8_t *data, unsigned len, struct image *out, int max_w, int max_h)
{
    out->px = 0;
    out->w = out->h = 0;
    out->alpha = 0;
    struct dom_doc *d = dom_parse((const char *)data, len, 1, 0);
    if (!d)
        return IMG_ENOMEM;
    struct dom_node *n = find_svg(d->root, 0);
    int w = 0, h = 0, err = IMG_EFORMAT;
    if (n) {
        int iw, ih;
        svg_size(n, &iw, &ih);
        double aw = anum(n, "width", -1, iw), ah = anum(n, "height", -1, ih);
        w = aw > 0 ? (int)(aw + 0.5) : iw;
        h = ah > 0 ? (int)(ah + 0.5) : aw > 0 && iw ? (int)(aw * ih / iw + 0.5) : ih;
        if (aw <= 0 && ah > 0 && ih)
            w = (int)(ah * iw / ih + 0.5);
        if (w > max_w && w > 0) {
            h = (int)((int64_t)h * max_w / w);
            w = max_w;
        }
        if (h > max_h && h > 0) {
            w = (int)((int64_t)w * max_h / h);
            h = max_h;
        }
        err = IMG_ETOOBIG;
        if (w > 0 && h > 0 && (int64_t)w * h <= CANVAS_MAX_PIXELS) {
            struct dom_canvas cv = { w, h, kzalloc((size_t)w * (size_t)h * 4), 0 };
            err = IMG_ENOMEM;
            if (cv.px) {
                draw(n, &cv, w, h);
                out->px = cv.px;
                out->w = w;
                out->h = h;
                out->alpha = 1;
                err = 0;
            }
        }
    }
    dom_free(d);
    return err;
}

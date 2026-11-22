/* jstest.exe - uruchamia JavaScript NaszOS na Windowsie.
 *   jstest plik.js [plik2.js ...]   - skrypty bez strony (kazdy w nowym swiecie JS)
 *   jstest strona.html              - strona: DOM, CSS, skrypty, DOMContentLoaded,
 *                                     timery przez 2 s, na koncu tekst <body>
 *   jstest -canvas obraz.bmp strona.html - to samo, a pierwsze <canvas> zapisuje do BMP
 *   jstest -render strona.bmp strona.html - uklada strone (1000 px) i rysuje ja do BMP
 *   jstest -e "kod"                 - jedno wyrazenie, wypisuje wynik */

#include <stdint.h>
#include "js.h"
#include "dom.h"
#include "css.h"
#include "html.h"
#include "string.h"
#include "timer.h"
#include "layout.h"
#include "heap.h"
#include "gfx.h"
#include "font.h"

void outs(const char *s);
char *read_file(const char *path, unsigned *len);
void sleep_ms(uint32_t ms);
char *command_line(void);
void exit_process(uint32_t code);
int write_file(const char *path, const void *data, unsigned len);

static int errors;
static const char *canvas_out;           /* -canvas plik.bmp */
static const char *render_out;           /* -render plik.bmp */

/* Pierwsze plotno strony -> BMP 32-bit (przezroczystosc na szachownicy, jak w edytorach) */
static void dump_canvas(struct dom_doc *d)
{
    const struct dom_canvas *c = d->canvases;
    while (c && c->next)                 /* lista ma najnowsze na poczatku - pierwsze jest na koncu */
        c = c->next;
    if (!c || !c->px) {
        outs("[canvas] brak plotna do zapisania\n");
        errors++;
        return;
    }
    unsigned size = 54 + (unsigned)c->w * (unsigned)c->h * 4;
    uint8_t *b = kmalloc(size);
    memset(b, 0, 54);
    b[0] = 'B'; b[1] = 'M';
    uint32_t v[] = { size, 0, 54, 40, (uint32_t)c->w, (uint32_t)(-c->h), 1 | 32 << 16, 0, size - 54 };
    memcpy(b + 2, &v[0], 4);
    memcpy(b + 10, &v[2], 4);
    memcpy(b + 14, &v[3], 4);
    memcpy(b + 18, &v[4], 4);
    memcpy(b + 22, &v[5], 4);
    memcpy(b + 26, &v[6], 4);
    memcpy(b + 34, &v[8], 4);
    uint32_t *px = (uint32_t *)(b + 54);
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            uint32_t p = c->px[y * c->w + x], a = p >> 24;
            uint32_t bg = ((x / 8 + y / 8) & 1) ? 0xCCCCCC : 0xFFFFFF;
            uint32_t r = ((p >> 16 & 255) * a + (bg >> 16 & 255) * (255 - a)) / 255;
            uint32_t g = ((p >> 8 & 255) * a + (bg >> 8 & 255) * (255 - a)) / 255;
            uint32_t bl = ((p & 255) * a + (bg & 255) * (255 - a)) / 255;
            px[y * c->w + x] = r << 16 | g << 8 | bl;
        }
    if (write_file(canvas_out, b, size) == 0) {
        outs("[canvas] zapisano ");
        outs(canvas_out);
        outs("\n");
    }
}

static void save_bmp(const char *path, const uint32_t *px, int w, int h)
{
    unsigned size = 54 + (unsigned)w * (unsigned)h * 4;
    uint8_t *b = kmalloc(size);
    memset(b, 0, 54);
    b[0] = 'B'; b[1] = 'M';
    uint32_t v[] = { size, 0, 54, 40, (uint32_t)w, (uint32_t)(-h), 1 | 32 << 16, 0, size - 54 };
    memcpy(b + 2, &v[0], 4);
    memcpy(b + 10, &v[2], 4);
    memcpy(b + 14, &v[3], 4);
    memcpy(b + 18, &v[4], 4);
    memcpy(b + 22, &v[5], 4);
    memcpy(b + 26, &v[6], 4);
    memcpy(b + 34, &v[8], 4);
    memcpy(b + 54, px, (size_t)w * (size_t)h * 4);
    if (write_file(path, b, size) == 0) {
        outs("[render] zapisano ");
        outs(path);
        outs("\n");
    }
    kfree(b);
}

/* Strona -> obrazek: te same funkcje rysujace co przegladarka (bez obrazkow - tylko ich miejsce) */
static void render_page(struct dom_doc *d, struct css_ctx *c)
{
    struct page_layout L;
    memset(&L, 0, sizeof(L));
    css_compute(c, d);
    layout_page(&L, d, 1000, 0, 0);
    int w = 1000, h = L.height + 20;
    if (h > 4000)
        h = 4000;
    if (h < 100)
        h = 100;
    uint32_t *px = kmalloc((size_t)w * (size_t)h * 4);
    gfx_use_buffer(px, w, h);
    gfx_fill(0, 0, w, h, L.bg);
    for (unsigned i = 0; i < L.n; i++) {
        const struct dl_item *it = &L.items[i];
        int has_rad = it->rad[0] | it->rad[1] | it->rad[2] | it->rad[3];
        switch (it->kind) {
        case DL_RECT:
            if (has_rad)
                gfx_fill_round(it->x, it->y, it->w, it->h, it->rad, it->color);
            else
                gfx_fill(it->x, it->y, it->w, it->h, it->color);
            break;
        case DL_RING:
            gfx_ring_round(it->x, it->y, it->w, it->h, it->rad, it->ext, it->color);
            break;
        case DL_SHADOW: {
            int s = it->ext[1];
            int16_t r[4];
            for (int k = 0; k < 4; k++)
                r[k] = (int16_t)(it->rad[k] ? (it->rad[k] + s > 0 ? it->rad[k] + s : 0) : 0);
            gfx_shadow(it->x + it->ext[2] - s, it->y + it->ext[3] - s, it->w + 2 * s, it->h + 2 * s, r,
                       it->ext[0], it->color, it->alpha, it->x, it->y, it->w, it->h, it->rad);
            break;
        }
        case DL_TEXT:
            if (it->font != FONT_MONO)
                font_draw(it->font, it->x, it->y, it->text, it->len, it->color);
            break;
        case DL_IMAGE:
            if (has_rad)
                gfx_fill_round(it->x, it->y, it->w, it->h, it->rad, 0x8899AA);
            else
                gfx_fill(it->x, it->y, it->w, it->h, 0x8899AA);
            break;
        }
    }
    save_bmp(render_out, px, w, h);
    kfree(px);
    layout_free(&L);
}

static void console(void *ctx, int level, const char *text)
{
    (void)ctx;
    outs(level == 2 ? "[blad] " : level == 1 ? "[uwaga] " : "");
    outs(text);
    outs("\n");
    if (level == 2)
        errors++;
}

static void alert(void *ctx, const char *text)
{
    (void)ctx;
    outs("[alert] ");
    outs(text);
    outs("\n");
}

static void navigate(void *ctx, const char *url)
{
    (void)ctx;
    outs("[navigate] ");
    outs(url);
    outs("\n");
}

static int ends_with(const char *s, const char *e)
{
    unsigned a = (unsigned)strlen(s), b = (unsigned)strlen(e);
    return a >= b && strcmp(s + a - b, e) == 0;
}

static char page_dir[256];

/* Skrypty z src= w tescie: pliki obok strony (http://localhost/plik.js -> katalog/plik.js) */
static int fetch(void *ctx, const char *url, char **body, unsigned *len)
{
    (void)ctx;
    const char *name = url;
    for (const char *p = url; *p; p++)
        if (*p == '/')
            name = p + 1;
    char path[512];
    ksnprintf(path, sizeof(path), "%s%s", page_dir, name);
    *body = read_file(path, len);
    return *body ? 0 : -1;
}

/* "Siec" testu dla fetch / XMLHttpRequest:
 *   .../echo  - odsyla metode, naglowki zapytania i tresc (sprawdzamy POST)
 *   .../fail  - blad sieci (status 0)
 *   inne      - plik obok strony testowej (404, gdy go nie ma) */
static void serve_requests(struct js_rt *rt)
{
    struct js_request q;
    while (js_take_request(rt, &q)) {
        const char *name = q.url;
        for (const char *p = q.url; *p; p++)
            if (*p == '/')
                name = p + 1;
        char hdr[512];
        if (strcmp(name, "fail") == 0) {
            js_request_done(rt, q.id, 0, 0, 0, 0, 0);
        } else if (strcmp(name, "echo") == 0) {
            static char out[8192];
            ksnprintf(out, sizeof(out), "%s|%s|%s", q.method, q.headers ? q.headers : "", q.body ? q.body : "");
            ksnprintf(hdr, sizeof(hdr), "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nX-Test: tak\r\nX-Test: drugi");
            js_request_done(rt, q.id, 200, hdr, out, (unsigned)strlen(out), q.url);
        } else {
            char path[512];
            unsigned len = 0;
            ksnprintf(path, sizeof(path), "%s%s", page_dir, name);
            char *body = read_file(path, &len);
            int json = ends_with(name, ".json");
            ksnprintf(hdr, sizeof(hdr), "HTTP/1.0 %s\r\nContent-Type: %s", body ? "200 OK" : "404 Not Found",
                      json ? "application/json" : "text/plain");
            js_request_done(rt, q.id, body ? 200 : 404, hdr, body ? body : "nie ma", body ? len : 6, q.url);
        }
        kfree(q.headers);
        kfree(q.body);
    }
}

struct page {
    struct js_rt *rt;
    struct js_host host;
    int n;
    const char *path;
    struct dom_doc *doc;
    struct css_ctx *css;
    struct page_layout lay;
};

static int inside(const struct dom_node *n, const struct dom_node *anc)
{
    for (; n; n = n->parent)
        if (n == anc)
            return 1;
    return 0;
}

/* Polozenie elementu po ulozeniu strony (jak w przegladarce) */
static int box(void *ctx, struct dom_node *n, int *x, int *y, int *w, int *h)
{
    struct page *pg = ctx;
    if (!pg->doc || !pg->css)
        return 0;
    css_compute(pg->css, pg->doc);
    layout_page(&pg->lay, pg->doc, 1000, 0, 0);
    int x1 = 1 << 30, y1 = 1 << 30, x2 = -(1 << 30), y2 = -(1 << 30);
    for (unsigned i = 0; i < pg->lay.n; i++) {
        const struct dl_item *it = &pg->lay.items[i];
        if (!it->node || !inside(it->node, n) || it->w <= 0 || it->h <= 0)
            continue;
        if (it->x < x1) x1 = it->x;
        if (it->y < y1) y1 = it->y;
        if (it->x + it->w > x2) x2 = it->x + it->w;
        if (it->y + it->h > y2) y2 = it->y + it->h;
    }
    if (x2 < x1)
        return 0;
    *x = x1; *y = y1; *w = x2 - x1; *h = y2 - y1;
    return 1;
}

/* Parser natrafil na </script> - wykonujemy go od razu (strona tylko do tego miejsca) */
static void on_script(void *ctx, struct dom_doc *d, struct dom_node *script)
{
    struct page *pg = ctx;
    if (!pg->rt)
        pg->rt = js_new(d, 0, "http://localhost/test.html", &pg->host);
    char name[64];
    ksnprintf(name, sizeof(name), "%s#%d", pg->path, ++pg->n);
    js_run_script(pg->rt, script, name);
}

static void run_page(const char *path, char *src, unsigned len)
{
    struct page pg;
    memset(&pg, 0, sizeof(pg));
    pg.path = path;
    pg.host.console = console;
    pg.host.alert = alert;
    pg.host.navigate = navigate;
    pg.host.fetch = fetch;
    pg.host.box = box;
    pg.host.ctx = &pg;
    ksnprintf(page_dir, sizeof(page_dir), "%s", path);
    for (int i = (int)strlen(page_dir) - 1; i >= 0; i--)
        if (page_dir[i] == '/' || page_dir[i] == '\\') {
            page_dir[i + 1] = 0;
            break;
        } else if (i == 0) {
            page_dir[0] = 0;
        }
    pg.host.view_w = 1000;
    pg.host.view_h = 700;
    struct dom_doc *d = dom_parse_scripts(src, len, 1, HC_UTF8, on_script, &pg);
    struct css_ctx *c = css_new(1000);
    if (d->css)
        css_add(c, d->css, d->css_len);
    css_compute(c, d);
    pg.doc = d;
    pg.css = c;
    struct js_rt *rt = pg.rt ? pg.rt : js_new(d, 0, "http://localhost/test.html", &pg.host);
    js_fire_loaded(rt);
    uint32_t end = timer_uptime_ms() + 2000;
    while (js_has_timers(rt) && timer_uptime_ms() < end) {
        js_timers(rt);
        serve_requests(rt);
        sleep_ms(5);
    }
    if (js_dirty(rt))
        css_compute(c, d);
    /* wynik: tekst strony */
    char out[4096];
    if (js_eval_text(rt, "document.body ? document.body.innerText : ''", 44, out, sizeof(out)) == 0) {
        outs("--- tekst strony ---\n");
        outs(out);
        outs("\n");
    }
    if (canvas_out)
        dump_canvas(d);
    if (render_out)
        render_page(d, c);
    js_free(rt);
}

void start(void)
{
    char *cl = command_line();
    /* argumenty: pomijamy nazwe programu */
    char *args[32];
    int n = 0;
    char *p = cl;
    while (*p) {
        while (*p == ' ')
            p++;
        if (!*p)
            break;
        char *a = p;
        if (*p == '"') {
            a = ++p;
            while (*p && *p != '"')
                p++;
        } else {
            while (*p && *p != ' ')
                p++;
        }
        if (*p)
            *p++ = '\0';
        if (n < 32)
            args[n++] = a;
    }
    if (n >= 3 && strcmp(args[1], "-e") == 0) {
        struct js_host host;
        memset(&host, 0, sizeof(host));
        host.console = console;
        struct js_rt *rt = js_new(0, 0, "", &host);
        char out[8192];
        int r = js_eval_text(rt, args[2], (unsigned)strlen(args[2]), out, sizeof(out));
        if (r == 0) {
            outs(out);
            outs("\n");
        }
        exit_process(r ? 1 : 0);
    }
    for (int i = 1; i < n; i++) {
        if (strcmp(args[i], "-canvas") == 0 && i + 1 < n) {
            canvas_out = args[++i];
            continue;
        }
        if (strcmp(args[i], "-render") == 0 && i + 1 < n) {
            render_out = args[++i];
            continue;
        }
        unsigned len;
        char *src = read_file(args[i], &len);
        if (!src) {
            outs("Nie moge otworzyc ");
            outs(args[i]);
            outs("\n");
            errors++;
            continue;
        }
        if (ends_with(args[i], ".html")) {
            run_page(args[i], src, len);
            continue;
        }
        struct js_host host;
        memset(&host, 0, sizeof(host));
        host.console = console;
        host.alert = alert;
        struct js_rt *rt = js_new(0, 0, "", &host);
        if (!rt) {
            outs("js_new nie wyszlo\n");
            exit_process(2);
        }
        js_run(rt, src, len, args[i], 0);
        uint32_t end = timer_uptime_ms() + 1000;
        while (js_has_timers(rt) && timer_uptime_ms() < end) {
            js_timers(rt);
            serve_requests(rt);
            sleep_ms(2);
        }
        js_free(rt);
    }
    exit_process(errors ? 1 : 0);
}

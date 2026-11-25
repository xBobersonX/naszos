/* Grafika pulpitu - patrz gfx.h. Trzy bufory:
 *   front      - bufor ramki karty (to widac na ekranie),
 *   back       - bufor roboczy, w nim skladamy obraz,
 *   background - gotowe tlo pulpitu, kopiowane do back przed rysowaniem okien.
 * Kursor rysujemy prosto na front operacja XOR: drugi XOR w tym samym
 * miejscu przywraca oryginalne piksele, wiec nie trzeba ich zapamietywac. */

#include "gfx.h"
#include "css.h"
#include "vga.h"
#include "heap.h"

#define CURSOR_ARM 9            /* dlugosc ramienia krzyzyka w pikselach */
#define GLYPH_W 8
#define GLYPH_H 16

static uint32_t *front, *back, *background, *target;
static unsigned W, H, front_pitch;
static struct rect clip;
static const uint8_t *font;

static int cursor_shown, cursor_x, cursor_y;

static void fill32(uint32_t *dst, uint32_t value, unsigned long n)
{
    __asm__ volatile ("cld; rep stosl" : "+D"(dst), "+c"(n) : "a"(value) : "memory");
}

static void copy32(uint32_t *dst, const uint32_t *src, unsigned long n)
{
    __asm__ volatile ("cld; rep movsl" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}

int gfx_begin(void)
{
    if (!vga_is_graphics())
        return 0;
    vga_framebuffer(&front, &W, &H, &front_pitch);
    back = kmalloc(W * H * 4);
    background = kmalloc(W * H * 4);
    if (!back || !background) {
        kfree(back);
        kfree(background);
        back = background = 0;
        return 0;
    }
    font = vga_font();
    target = back;
    gfx_set_clip(0);
    cursor_shown = 0;
    vga_suspend(1);
    return 1;
}

static void cursor_xor(void);

void gfx_end(void)
{
    if (cursor_shown)
        cursor_xor();
    cursor_shown = 0;
    kfree(back);
    kfree(background);
    back = background = 0;
    vga_suspend(0);
}

int gfx_width(void)  { return (int)W; }
int gfx_height(void) { return (int)H; }

void gfx_target_background(int on)
{
    target = on ? background : back;
}

/* ---- prostokaty ---- */

int rect_contains(const struct rect *r, int x, int y)
{
    return x >= r->x && y >= r->y && x < r->x + r->w && y < r->y + r->h;
}

struct rect rect_union(struct rect a, struct rect b)
{
    if (a.w <= 0 || a.h <= 0)
        return b;
    if (b.w <= 0 || b.h <= 0)
        return a;
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    struct rect r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}

/* Przycina prostokat do obszaru obcinania; 0 = nic nie zostalo */
static int clip_rect(int *x, int *y, int *w, int *h)
{
    int x0 = *x > clip.x ? *x : clip.x, y0 = *y > clip.y ? *y : clip.y;
    int x1 = *x + *w < clip.x + clip.w ? *x + *w : clip.x + clip.w;
    int y1 = *y + *h < clip.y + clip.h ? *y + *h : clip.y + clip.h;
    if (x1 <= x0 || y1 <= y0)
        return 0;
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return 1;
}

/* Rysowanie do wlasnego bufora zamiast ekranu (test na PC: jstest -render) */
void gfx_use_buffer(uint32_t *px, int w, int h)
{
    target = back = px;
    W = (unsigned)w;
    H = (unsigned)h;
    clip.x = clip.y = 0;
    clip.w = w;
    clip.h = h;
}

void gfx_set_clip(const struct rect *r)
{
    struct rect screen = { 0, 0, (int)W, (int)H };
    clip = screen;
    if (r) {
        int x = r->x, y = r->y, w = r->w, h = r->h;
        if (clip_rect(&x, &y, &w, &h)) {
            clip.x = x; clip.y = y; clip.w = w; clip.h = h;
        } else {
            clip.w = clip.h = 0;
        }
    }
}

struct rect gfx_get_clip(void)
{
    return clip;
}

/* ---- rysowanie ---- */

void gfx_fill(int x, int y, int w, int h, uint32_t color)
{
    if (!clip_rect(&x, &y, &w, &h))
        return;
    for (int row = y; row < y + h; row++)
        fill32(target + row * W + x, color, (uint32_t)w);
}

void gfx_frame(int x, int y, int w, int h, uint32_t color)
{
    gfx_fill(x, y, w, 1, color);
    gfx_fill(x, y + h - 1, w, 1, color);
    gfx_fill(x, y, 1, h, color);
    gfx_fill(x + w - 1, y, 1, h, color);
}

uint32_t gfx_mix(uint32_t a, uint32_t b, int num, int den)
{
    uint32_t r = ((a >> 16 & 0xFF) * (uint32_t)(den - num) + (b >> 16 & 0xFF) * (uint32_t)num) / (uint32_t)den;
    uint32_t g = ((a >> 8 & 0xFF) * (uint32_t)(den - num) + (b >> 8 & 0xFF) * (uint32_t)num) / (uint32_t)den;
    uint32_t bl = ((a & 0xFF) * (uint32_t)(den - num) + (b & 0xFF) * (uint32_t)num) / (uint32_t)den;
    return r << 16 | g << 8 | bl;
}

void gfx_gradient(int x, int y, int w, int h, uint32_t top, uint32_t bottom)
{
    for (int row = 0; row < h; row++)
        gfx_fill(x, y + row, w, 1, gfx_mix(top, bottom, row, h > 1 ? h - 1 : 1));
}

void gfx_darken(int x, int y, int w, int h)
{
    if (!clip_rect(&x, &y, &w, &h))
        return;
    for (int row = y; row < y + h; row++)
        for (uint32_t *p = target + row * W + x, *end = p + w; p < end; p++)
            *p = (*p >> 1) & 0x7F7F7F;
}

void gfx_lighten(int x, int y, int w, int h)
{
    if (!clip_rect(&x, &y, &w, &h))
        return;
    for (int row = y; row < y + h; row++)
        for (uint32_t *p = target + row * W + x, *end = p + w; p < end; p++)
            *p = ((*p >> 1) & 0x7F7F7F) + 0x606060;
}

void gfx_text_scaled(int x, int y, const char *s, uint32_t color, int scale)
{
    for (; *s; s++, x += GLYPH_W * scale) {
        const uint8_t *glyph = &font[(uint8_t)*s * GLYPH_H];
        for (int row = 0; row < GLYPH_H; row++)
            for (int col = 0; col < GLYPH_W; col++)
                if (glyph[row] & (0x80 >> col)) {
                    int px = x + col * scale, py = y + row * scale;
                    if (scale == 1) {
                        if (rect_contains(&clip, px, py))
                            target[py * W + px] = color;
                    } else {
                        gfx_fill(px, py, scale, scale, color);
                    }
                }
    }
}

void gfx_blit_scaled(int x, int y, int w, int h, const uint32_t *pixels, int sw, int sh)
{
    if (w <= 0 || h <= 0 || sw <= 0 || sh <= 0)
        return;
    int x0 = x > clip.x ? x : clip.x, y0 = y > clip.y ? y : clip.y;
    int x1 = x + w < clip.x + clip.w ? x + w : clip.x + clip.w;
    int y1 = y + h < clip.y + clip.h ? y + h : clip.y + clip.h;
    for (int py = y0; py < y1; py++) {                 /* najblizszy piksel oryginalu */
        const uint32_t *row = pixels + (py - y) * sh / h * sw;
        uint32_t *p = target + py * W;
        for (int px = x0; px < x1; px++)
            p[px] = row[(px - x) * sw / w];
    }
}

void gfx_blit_alpha(int x, int y, int w, int h, const uint32_t *pixels, int sw, int sh)
{
    if (w <= 0 || h <= 0 || sw <= 0 || sh <= 0)
        return;
    int x0 = x > clip.x ? x : clip.x, y0 = y > clip.y ? y : clip.y;
    int x1 = x + w < clip.x + clip.w ? x + w : clip.x + clip.w;
    int y1 = y + h < clip.y + clip.h ? y + h : clip.y + clip.h;
    for (int py = y0; py < y1; py++) {
        const uint32_t *row = pixels + (int64_t)(py - y) * sh / h * sw;
        uint32_t *p = target + py * W;
        for (int px = x0; px < x1; px++) {
            uint32_t s = row[(int64_t)(px - x) * sw / w], k = s >> 24;
            if (!k)
                continue;
            if (k == 255) {
                p[px] = s & 0xFFFFFF;
                continue;
            }
            uint32_t d = p[px], inv = 255 - k;       /* kolor * alfa + tlo * (1 - alfa) */
            p[px] = (((s >> 16 & 255) * k + (d >> 16 & 255) * inv) / 255) << 16 |
                    (((s >> 8 & 255) * k + (d >> 8 & 255) * inv) / 255) << 8 |
                    (((s & 255) * k + (d & 255) * inv) / 255);
        }
    }
}

static uint32_t text_mul = 255;
void gfx_text_alpha(int a) { text_mul = a < 0 ? 0 : a > 255 ? 255 : (uint32_t)a; }

void gfx_alpha_mask(int x, int y, int w, int h, const uint8_t *alpha, uint32_t color)
{
    int x0 = x > clip.x ? x : clip.x, y0 = y > clip.y ? y : clip.y;
    int x1 = x + w < clip.x + clip.w ? x + w : clip.x + clip.w;
    int y1 = y + h < clip.y + clip.h ? y + h : clip.y + clip.h;
    uint32_t cr = color >> 16 & 255, cg = color >> 8 & 255, cb = color & 255;
    for (int py = y0; py < y1; py++) {
        const uint8_t *a = alpha + (py - y) * w;
        uint32_t *p = target + py * W;
        for (int px = x0; px < x1; px++) {
            uint32_t k = a[px - x];
            if (text_mul < 255)
                k = k * text_mul / 255;
            if (!k)
                continue;
            if (k == 255) {
                p[px] = color;
                continue;
            }
            uint32_t d = p[px], inv = 255 - k;       /* kolor * k + tlo * (255 - k) */
            p[px] = ((cr * k + (d >> 16 & 255) * inv) / 255) << 16 |
                    ((cg * k + (d >> 8 & 255) * inv) / 255) << 8 |
                    ((cb * k + (d & 255) * inv) / 255);
        }
    }
}

void gfx_char(int x, int y, unsigned char ch, uint32_t fg, uint32_t bg)
{
    const uint8_t *glyph = &font[ch * GLYPH_H];
    int inside = x >= clip.x && y >= clip.y &&
                 x + GLYPH_W <= clip.x + clip.w && y + GLYPH_H <= clip.y + clip.h;
    if (!inside) {                      /* na krawedzi obcinania - wolniej, piksel po pikselu */
        for (int row = 0; row < GLYPH_H; row++)
            for (int col = 0; col < GLYPH_W; col++)
                if (rect_contains(&clip, x + col, y + row))
                    target[(y + row) * W + x + col] = (glyph[row] & (0x80 >> col)) ? fg : bg;
        return;
    }
    for (int row = 0; row < GLYPH_H; row++) {
        uint32_t *p = target + (y + row) * W + x;
        uint8_t bits = glyph[row];
        for (int col = 0; col < GLYPH_W; col++)
            p[col] = (bits & (0x80 >> col)) ? fg : bg;
    }
}

void gfx_text(int x, int y, const char *s, uint32_t color)
{
    gfx_text_scaled(x, y, s, color, 1);
}

int gfx_text_width(const char *s)
{
    int n = 0;
    while (*s++)
        n++;
    return n * GLYPH_W;
}

void gfx_blit(int x, int y, int w, int h, const uint32_t *pixels)
{
    int cx = x, cy = y, cw = w, ch = h;
    if (!clip_rect(&cx, &cy, &cw, &ch))
        return;
    for (int row = cy; row < cy + ch; row++)
        copy32(target + row * W + cx, pixels + (row - y) * w + (cx - x), (uint32_t)cw);
}

void gfx_copy_background(void)
{
    for (int row = clip.y; row < clip.y + clip.h; row++)
        copy32(back + row * W + clip.x, background + row * W + clip.x, (uint32_t)clip.w);
}

/* ---- kursor i wyswietlanie ---- */

static void xor_pixel(int x, int y)
{
    if (x >= 0 && y >= 0 && x < (int)W && y < (int)H)
        front[y * front_pitch + x] ^= 0xFFFFFF;
}

/* Krzyzyk 2 px grubosci. Srodek nalezy do obu ramion, ale rysujemy go raz -
 * drugi XOR przywrocilby tam oryginalny kolor */
static void cursor_xor(void)
{
    for (int dx = -CURSOR_ARM; dx <= CURSOR_ARM; dx++)
        for (int dy = -1; dy <= 0; dy++)
            xor_pixel(cursor_x + dx, cursor_y + dy);
    for (int dy = -CURSOR_ARM; dy <= CURSOR_ARM; dy++) {
        if (dy == -1 || dy == 0)
            continue;
        for (int dx = -1; dx <= 0; dx++)
            xor_pixel(cursor_x + dx, cursor_y + dy);
    }
}

void gfx_cursor_move(int x, int y)
{
    if (cursor_shown && x == cursor_x && y == cursor_y)
        return;
    if (cursor_shown)
        cursor_xor();                   /* zdejmujemy ze starego miejsca */
    cursor_x = x;
    cursor_y = y;
    cursor_xor();
    cursor_shown = 1;
}

void gfx_present(const struct rect *r)
{
    int x = r->x, y = r->y, w = r->w, h = r->h;
    struct rect saved = clip;
    gfx_set_clip(0);
    int visible = clip_rect(&x, &y, &w, &h);
    clip = saved;
    if (!visible)
        return;

    int had_cursor = cursor_shown;
    if (had_cursor)
        cursor_xor();                   /* kopiowanie nadpisze piksele pod kursorem */
    for (int row = y; row < y + h; row++)
        copy32(front + row * front_pitch + x, back + row * W + x, (uint32_t)w);
    if (had_cursor)
        cursor_xor();
}

/* ---------- zaokraglone rogi i cienie (CSS border-radius, box-shadow) ----------
 * Liczymy w 1/16 piksela i bez liczb zmiennoprzecinkowych. "Pokrycie" piksela
 * (0..256) mowi, jaka czesc piksela lezy w ksztalcie - dzieki temu brzegi rogow
 * sa gladkie, a nie schodkowe. Rogi: r[0] lewy-gorny, r[1] prawy-gorny,
 * r[2] prawy-dolny, r[3] lewy-dolny. */

static uint32_t isqrt64(uint64_t v)
{
    uint64_t r = 0, bit = (uint64_t)1 << 62;
    while (bit > v)
        bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

static inline void blend_px(uint32_t *p, uint32_t color, uint32_t a)   /* a: 0..256 */
{
    if (a >= 256) {
        *p = color;
        return;
    }
    uint32_t d = *p, inv = 256 - a;
    *p = (((color >> 16 & 255) * a + (d >> 16 & 255) * inv) >> 8) << 16 |
         (((color >> 8 & 255) * a + (d >> 8 & 255) * inv) >> 8) << 8 |
         (((color & 255) * a + (d & 255) * inv) >> 8);
}

/* Odleglosc (1/16 px, ze znakiem: ujemna = w srodku) punktu od prostokata z zaokraglonymi rogami */
static int rr_dist(int px16, int py16, int x, int y, int w, int h, const int16_t *r)
{
    int cx16 = x * 16 + w * 8, cy16 = y * 16 + h * 8;
    int dx = px16 - cx16, dy = py16 - cy16;
    int rad = dy < 0 ? (dx < 0 ? r[0] : r[1]) : (dx < 0 ? r[3] : r[2]);
    int qx = (dx < 0 ? -dx : dx) - (w * 8 - rad * 16);
    int qy = (dy < 0 ? -dy : dy) - (h * 8 - rad * 16);
    int out;
    if (qx > 0 && qy > 0)                                /* w obszarze rogu - odleglosc od okregu */
        out = (int)isqrt64((uint64_t)((int64_t)qx * qx + (int64_t)qy * qy));
    else                                                 /* przy prostym boku albo w srodku */
        out = qx > qy ? qx : qy;
    return out - rad * 16;
}

/* Pokrycie piksela (px, py) przez ksztalt: 0..256 */
static inline int rr_cover(int px, int py, int x, int y, int w, int h, const int16_t *r)
{
    if (w <= 0 || h <= 0 || px < x || py < y || px >= x + w || py >= y + h)
        return 0;
    int d = rr_dist(px * 16 + 8, py * 16 + 8, x, y, w, h, r);
    int c = 128 - d * 16;                                /* brzeg rozmyty na 1 piksel */
    return c < 0 ? 0 : c > 256 ? 256 : c;
}

static int in_corner(int px, int py, int x, int y, int w, int h, const int16_t *r)
{
    return (py < y + r[0] && px < x + r[0]) || (py < y + r[1] && px >= x + w - r[1]) ||
           (py >= y + h - r[2] && px >= x + w - r[2]) || (py >= y + h - r[3] && px < x + r[3]);
}

void gfx_fill_round(int x, int y, int w, int h, const int16_t r[4], uint32_t color)
{
    int x0 = x > clip.x ? x : clip.x, y0 = y > clip.y ? y : clip.y;
    int x1 = x + w < clip.x + clip.w ? x + w : clip.x + clip.w;
    int y1 = y + h < clip.y + clip.h ? y + h : clip.y + clip.h;
    int top = r[0] > r[1] ? r[0] : r[1], bot = r[2] > r[3] ? r[2] : r[3];
    for (int py = y0; py < y1; py++) {
        uint32_t *p = target + py * W;
        if (py >= y + top && py < y + h - bot) {         /* srodkowe wiersze - bez rogow */
            for (int px = x0; px < x1; px++)
                p[px] = color;
            continue;
        }
        for (int px = x0; px < x1; px++) {
            if (!in_corner(px, py, x, y, w, h, r)) {
                p[px] = color;
                continue;
            }
            int c = rr_cover(px, py, x, y, w, h, r);
            if (c)
                blend_px(&p[px], color, (uint32_t)c);
        }
    }
}

/* Ramka o grubosciach b[4] (gora, prawo, dol, lewo) wokol zaokraglonego prostokata */
void gfx_ring_round(int x, int y, int w, int h, const int16_t r[4], const int16_t b[4], uint32_t color)
{
    int ix = x + b[3], iy = y + b[0], iw = w - b[1] - b[3], ih = h - b[0] - b[2];
    int16_t ir[4];
    ir[0] = (int16_t)(r[0] - (b[3] > b[0] ? b[3] : b[0]));
    ir[1] = (int16_t)(r[1] - (b[1] > b[0] ? b[1] : b[0]));
    ir[2] = (int16_t)(r[2] - (b[1] > b[2] ? b[1] : b[2]));
    ir[3] = (int16_t)(r[3] - (b[3] > b[2] ? b[3] : b[2]));
    for (int i = 0; i < 4; i++)
        if (ir[i] < 0)
            ir[i] = 0;
    int x0 = x > clip.x ? x : clip.x, y0 = y > clip.y ? y : clip.y;
    int x1 = x + w < clip.x + clip.w ? x + w : clip.x + clip.w;
    int y1 = y + h < clip.y + clip.h ? y + h : clip.y + clip.h;
    for (int py = y0; py < y1; py++) {
        uint32_t *p = target + py * W;
        for (int px = x0; px < x1; px++) {
            int inner = px >= ix && px < ix + iw && py >= iy && py < iy + ih;
            int oc = in_corner(px, py, x, y, w, h, r) ? rr_cover(px, py, x, y, w, h, r) : 256;
            int ic = !inner ? 0 : in_corner(px, py, ix, iy, iw, ih, ir) ? rr_cover(px, py, ix, iy, iw, ih, ir) : 256;
            int c = oc - ic;
            if (c > 0)
                blend_px(&p[px], color, (uint32_t)c);
        }
    }
}

/* Cien: ksztalt (x, y, w, h, r) rozmyty na blur px, kolor z przezroczystoscia alpha (0..255).
 * Wnetrze pudelka (bx, by, bw, bh, br) zostaje nietkniete - cien jest tylko na zewnatrz. */
void gfx_shadow(int x, int y, int w, int h, const int16_t r[4], int blur, uint32_t color, int alpha,
                int bx, int by, int bw, int bh, const int16_t br[4])
{
    if (w <= 0 || h <= 0 || alpha <= 0)
        return;
    int ext = blur + 1;
    int x0 = x - ext > clip.x ? x - ext : clip.x, y0 = y - ext > clip.y ? y - ext : clip.y;
    int x1 = x + w + ext < clip.x + clip.w ? x + w + ext : clip.x + clip.w;
    int y1 = y + h + ext < clip.y + clip.h ? y + h + ext : clip.y + clip.h;
    int span = blur * 16;                                /* przejscie od pelnego cienia do zera: [-blur, +blur] */
    for (int py = y0; py < y1; py++) {
        uint32_t *p = target + py * W;
        for (int px = x0; px < x1; px++) {
            int boxc = rr_cover(px, py, bx, by, bw, bh, br);
            if (boxc >= 256)
                continue;
            int d = rr_dist(px * 16 + 8, py * 16 + 8, x, y, w, h, r);
            int a;
            if (!blur) {
                a = 128 - d * 16;
                a = a < 0 ? 0 : a > 256 ? 256 : a;
            } else {
                if (d >= span)
                    continue;
                if (d <= -span) {
                    a = 256;
                } else {
                    /* gladkie przejscie (smoothstep): t = 0..256 */
                    int t = (d + span) * 128 / span;
                    a = 256 - (t * t * (768 - 2 * t) >> 16);
                }
            }
            a = a * alpha / 255 * (256 - boxc) >> 8;
            if (a > 0)
                blend_px(&p[px], color, (uint32_t)a);
        }
    }
}

/* Obrazek z zaokraglonymi rogami (np. okragly awatar: border-radius: 50%) */
void gfx_blit_round(int x, int y, int w, int h, const uint32_t *pixels, int sw, int sh, const int16_t r[4])
{
    if (w <= 0 || h <= 0 || sw <= 0 || sh <= 0)
        return;
    int x0 = x > clip.x ? x : clip.x, y0 = y > clip.y ? y : clip.y;
    int x1 = x + w < clip.x + clip.w ? x + w : clip.x + clip.w;
    int y1 = y + h < clip.y + clip.h ? y + h : clip.y + clip.h;
    for (int py = y0; py < y1; py++) {
        const uint32_t *row = pixels + (int64_t)(py - y) * sh / h * sw;
        uint32_t *p = target + py * W;
        for (int px = x0; px < x1; px++) {
            uint32_t s = row[(int64_t)(px - x) * sw / w] & 0xFFFFFF;
            if (!in_corner(px, py, x, y, w, h, r)) {
                p[px] = s;
                continue;
            }
            int c = rr_cover(px, py, x, y, w, h, r);
            if (c)
                blend_px(&p[px], s, (uint32_t)c);
        }
    }
}

/* ---------- gradienty CSS ---------- */

static const int16_t sin_tab[91] = { 0,71,143,214,286,357,428,499,570,641,711,782,852,921,991,1060,1129,1198,1266,1334,1401,1468,1534,1600,1666,1731,1796,1860,1923,1986,2048,2110,2171,2231,2290,2349,2408,2465,2522,2578,2633,2687,2741,2793,2845,2896,2946,2996,3044,3091,3138,3183,3228,3271,3314,3355,3396,3435,3474,3511,3547,3582,3617,3650,3681,3712,3742,3770,3798,3824,3849,3873,3896,3917,3937,3956,3974,3991,4006,4021,4034,4046,4056,4065,4074,4080,4086,4090,4094,4095,4096 };

static int isin(int d)                                   /* sin(d stopni) * 4096 */
{
    d %= 360;
    if (d < 0)
        d += 360;
    if (d <= 90) return sin_tab[d];
    if (d <= 180) return sin_tab[180 - d];
    if (d <= 270) return -sin_tab[d - 180];
    return -sin_tab[360 - d];
}

void gfx_fill_grad(int x, int y, int w, int h, const int16_t r[4], int type, int angle, int circle, int n,
                   const uint32_t *col, const uint8_t *alpha, const int16_t *pos)
{
    if (w <= 0 || h <= 0 || n < 1)
        return;
    /* tablica 256 kolorow wzdluz gradientu */
    uint32_t lc[256];
    uint16_t la[256];
    for (int i = 0; i < 256; i++) {
        int t = i * 1000 / 255, k = 0;
        while (k + 1 < n && pos[k + 1] < t)
            k++;
        int k2 = k + 1 < n ? k + 1 : k;
        int f = 0;
        if (t <= pos[0])
            k2 = k = 0;
        else if (t >= pos[n - 1])
            k = k2 = n - 1;
        else if (pos[k2] > pos[k])
            f = (t - pos[k]) * 256 / (pos[k2] - pos[k]);
        uint32_t a = col[k], b = col[k2];
        int rr = (int)((a >> 16) & 255) + ((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * f / 256;
        int gg = (int)((a >> 8) & 255) + ((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * f / 256;
        int bb = (int)(a & 255) + ((int)(b & 255) - (int)(a & 255)) * f / 256;
        lc[i] = (uint32_t)rr << 16 | (uint32_t)gg << 8 | (uint32_t)bb;
        la[i] = (uint16_t)((alpha[k] + (alpha[k2] - alpha[k]) * f / 256) * 256 / 255);
    }
    int s = isin(angle), c = isin(angle + 90);           /* kierunek (sin, -cos) */
    int64_t L = (int64_t)w * (s < 0 ? -s : s) + (int64_t)h * (c < 0 ? -c : c);   /* dlugosc linii * 4096 */
    if (L <= 0)
        L = 1;
    int rx, ry;
    if (circle) {
        rx = ry = (int)isqrt64((uint64_t)w * w + (uint64_t)h * h) / 2;
    } else {
        rx = w * 181 / 256;                              /* najdalszy rog: polowa * pierwiastek z 2 */
        ry = h * 181 / 256;
    }
    if (rx < 1) rx = 1;
    if (ry < 1) ry = 1;
    int16_t zero[4] = { 0, 0, 0, 0 };
    if (!r)
        r = zero;
    int x0 = x > clip.x ? x : clip.x, y0 = y > clip.y ? y : clip.y;
    int x1 = x + w < clip.x + clip.w ? x + w : clip.x + clip.w;
    int y1 = y + h < clip.y + clip.h ? y + h : clip.y + clip.h;
    int rounded = r[0] | r[1] | r[2] | r[3];
    for (int py = y0; py < y1; py++) {
        uint32_t *p = target + py * W;
        int dy2 = 2 * (py - y) - h + 1;                  /* od srodka, w polowkach piksela */
        for (int px = x0; px < x1; px++) {
            int dx2 = 2 * (px - x) - w + 1, t;
            if (type == 1) {
                int64_t proj = (int64_t)dx2 * s - (int64_t)dy2 * c;
                t = 128 + (int)(proj * 128 / L);
            } else {
                int64_t ux = (int64_t)dx2 * 128 / rx, uy = (int64_t)dy2 * 128 / ry;
                t = (int)isqrt64((uint64_t)(ux * ux + uy * uy));
            }
            if (t < 0) t = 0;
            if (t > 255) t = 255;
            uint32_t a = la[t];
            if (rounded && in_corner(px, py, x, y, w, h, r))
                a = a * (uint32_t)rr_cover(px, py, x, y, w, h, r) / 256;
            if (a)
                blend_px(&p[px], lc[t], a);
        }
    }
}

/* ---------- grupy: opacity i filter ---------- */


void gfx_read_rect(int x, int y, int w, int h, uint32_t *out)
{
    for (int py = 0; py < h; py++)
        for (int px = 0; px < w; px++)
            out[py * w + px] = target[(y + py) * W + x + px];
}

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* Piksele, ktore element zmienil, przechodza przez filtry i mieszaja sie ze starymi
 * z przezroczystoscia (stare = to, co bylo pod elementem) */
void gfx_group_done(int x, int y, int w, int h, const uint32_t *before, int alpha255, int nf,
                    const uint8_t *fop, const int16_t *famt)
{
    
    for (int py = 0; py < h; py++)
        for (int px = 0; px < w; px++) {
            uint32_t *p = &target[(y + py) * W + x + px], old = before[py * w + px], cur = *p & 0xFFFFFF;
            if (cur == (old & 0xFFFFFF))
                continue;
            int r = (int)(cur >> 16 & 255), g = (int)(cur >> 8 & 255), b = (int)(cur & 255), a = alpha255;
            for (int k = 0; k < nf; k++) {
                int m = famt[k], gray = (r * 299 + g * 587 + b * 114) / 1000;
                switch (fop[k]) {
                case FI_GRAY:
                    if (m > 1000) m = 1000;
                    r += (gray - r) * m / 1000; g += (gray - g) * m / 1000; b += (gray - b) * m / 1000;
                    break;
                case FI_SEPIA: {
                    if (m > 1000) m = 1000;
                    int sr = clamp255((r * 393 + g * 769 + b * 189) / 1000);
                    int sg = clamp255((r * 349 + g * 686 + b * 168) / 1000);
                    int sb = clamp255((r * 272 + g * 534 + b * 131) / 1000);
                    r += (sr - r) * m / 1000; g += (sg - g) * m / 1000; b += (sb - b) * m / 1000;
                    break;
                }
                case FI_INVERT:
                    if (m > 1000) m = 1000;
                    r += (255 - 2 * r) * m / 1000; g += (255 - 2 * g) * m / 1000; b += (255 - 2 * b) * m / 1000;
                    break;
                case FI_BRIGHT:
                    r = clamp255(r * m / 1000); g = clamp255(g * m / 1000); b = clamp255(b * m / 1000);
                    break;
                case FI_CONTRAST:
                    r = clamp255((r - 128) * m / 1000 + 128); g = clamp255((g - 128) * m / 1000 + 128);
                    b = clamp255((b - 128) * m / 1000 + 128);
                    break;
                case FI_SATURATE:
                    r = clamp255(gray + (r - gray) * m / 1000); g = clamp255(gray + (g - gray) * m / 1000);
                    b = clamp255(gray + (b - gray) * m / 1000);
                    break;
                case FI_OPACITY:
                    a = a * (m > 1000 ? 1000 : m) / 1000;
                    break;
                }
            }
            int orr = (int)(old >> 16 & 255), og = (int)(old >> 8 & 255), ob = (int)(old & 255);
            *p = (uint32_t)(orr + (r - orr) * a / 255) << 16 | (uint32_t)(og + (g - og) * a / 255) << 8 |
                 (uint32_t)(ob + (b - ob) * a / 255);
        }
}

/* Prostokat grupy przyciety do obszaru rysowania (0 = nic nie widac) */
int gfx_clip_rect(int *x, int *y, int *w, int *h)
{
    return clip_rect(x, y, w, h);
}

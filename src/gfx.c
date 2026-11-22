/* Grafika pulpitu - patrz gfx.h. Trzy bufory:
 *   front      - bufor ramki karty (to widac na ekranie),
 *   back       - bufor roboczy, w nim skladamy obraz,
 *   background - gotowe tlo pulpitu, kopiowane do back przed rysowaniem okien.
 * Kursor rysujemy prosto na front operacja XOR: drugi XOR w tym samym
 * miejscu przywraca oryginalne piksele, wiec nie trzeba ich zapamietywac. */

#include "gfx.h"
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

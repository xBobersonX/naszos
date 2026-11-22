/* Rysowanie ladnych czcionek - patrz font.h. Kazdy znak to mala bitmapa
 * z przezroczystoscia; kladziemy ja na tlo funkcja gfx_alpha_mask. */

#include "font.h"
#include "gfx.h"
#include "utf8.h"

static const struct font_glyph *find(const struct font_face *f, uint32_t cp)
{
    int lo = 0, hi = f->count - 1;
    while (lo <= hi) {                         /* znaki posortowane - szukanie polowkowe */
        int mid = (lo + hi) / 2;
        if (f->glyphs[mid].cp == cp)
            return &f->glyphs[mid];
        if (f->glyphs[mid].cp < cp)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return 0;
}

static const struct font_glyph *glyph(const struct font_face *f, uint32_t cp)
{
    const struct font_glyph *g = find(f, cp);
    if (!g && cp >= 0x80) {                    /* nie ma - moze chociaz litera bez ogonka */
        char a = unicode_to_ascii(cp);
        g = a ? find(f, (uint8_t)a) : 0;
    }
    return g ? g : find(f, '?');
}

const struct font_glyph *font_glyph(int font, uint32_t cp) { return glyph(&font_faces[font], cp); }

int font_height(int font) { return font_faces[font].height; }
int font_ascent(int font) { return font_faces[font].ascent; }

int font_width(int font, const char *utf8, unsigned len)
{
    const struct font_face *f = &font_faces[font];
    const char *s = utf8, *end = utf8 + len;
    int w = 0;
    while (s < end) {
        const struct font_glyph *g = glyph(f, utf8_next(&s, end));
        if (g)
            w += g->adv;
    }
    return w;
}

int font_draw(int font, int x, int y, const char *utf8, unsigned len, uint32_t color)
{
    const struct font_face *f = &font_faces[font];
    const char *s = utf8, *end = utf8 + len;
    int x0 = x;
    while (s < end) {
        const struct font_glyph *g = glyph(f, utf8_next(&s, end));
        if (!g)
            continue;
        if (g->w)
            gfx_alpha_mask(x + g->xoff, y + g->yoff, g->w, g->h, f->alpha + g->off, color);
        x += g->adv;
    }
    return x - x0;
}

unsigned font_fit(int font, const char *utf8, unsigned len, int max_w)
{
    const struct font_face *f = &font_faces[font];
    const char *s = utf8, *end = utf8 + len;
    int w = 0;
    while (s < end) {
        const char *before = s;
        const struct font_glyph *g = glyph(f, utf8_next(&s, end));
        if (g && w + g->adv > max_w)
            return (unsigned)(before - utf8);
        if (g)
            w += g->adv;
    }
    return len;
}

/* Wspolne kawalki rysowania strony (browser.c i tools/jstest): grupy dla
 * opacity / filter i cien tekstu (text-shadow).
 *
 * Grupa: przed narysowaniem elementu zapamietujemy piksele pod nim, a po
 * narysowaniu mieszamy nowe ze starymi (przezroczystosc) i przeliczamy kolory
 * (filtry). Dzieki temu caly element jest polprzezroczysty jak jedna warstwa. */
#ifndef PAINTGRP_H
#define PAINTGRP_H

#include "layout.h"
#include "css.h"
#include "gfx.h"
#include "font.h"
#include "heap.h"

#define PAINT_GROUPS 8

struct paint_group {
    unsigned end;                        /* pierwszy element za grupa */
    int x, y, w, h;
    uint32_t *save;
    const struct style *s;
};

/* Element i to DL_GROUP; ox, oy - przesuniecie strony na ekranie. 1 = grupa zaczeta */
static inline int paint_group_begin(const struct page_layout *L, unsigned i, int ox, int oy, struct paint_group *g)
{
    const struct dl_item *it = &L->items[i];
    g->end = i + 1 + (unsigned)it->link;
    if (g->end > L->n)
        g->end = L->n;
    if (!it->node || !it->node->style)
        return 0;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (unsigned k = i + 1; k < g->end; k++) {
        const struct dl_item *e = &L->items[k];
        if (e->kind == DL_GROUP || e->kind == DL_HIDDEN)
            continue;
        int grow = e->kind == DL_SHADOW ? e->ext[0] + e->ext[1] + 64 : e->kind == DL_TEXT ? 24 : 0;
        int w = e->w > 0 ? e->w : 0, h = e->h > 0 ? e->h : 0;
        if (ox + e->x - grow < x0) x0 = ox + e->x - grow;
        if (oy + e->y - grow < y0) y0 = oy + e->y - grow;
        if (ox + e->x + w + grow > x1) x1 = ox + e->x + w + grow;
        if (oy + e->y + h + grow > y1) y1 = oy + e->y + h + grow;
    }
    if (x1 <= x0 || y1 <= y0)
        return 0;
    g->x = x0;
    g->y = y0;
    g->w = x1 - x0;
    g->h = y1 - y0;
    if (!gfx_clip_rect(&g->x, &g->y, &g->w, &g->h))
        return 0;
    g->save = kmalloc((size_t)g->w * (size_t)g->h * 4);
    if (!g->save)
        return 0;
    g->s = it->node->style;
    gfx_read_rect(g->x, g->y, g->w, g->h, g->save);
    return 1;
}

static inline void paint_group_end(struct paint_group *g)
{
    gfx_group_done(g->x, g->y, g->w, g->h, g->save, 255 - g->s->transp, g->s->filt_n, g->s->filt_op,
                   g->s->filt_amt);
    kfree(g->save);
}

/* Cien pod tekstem (przed narysowaniem samego tekstu) */
static inline void paint_text_shadow(const struct dl_item *it, int x, int y)
{
    const struct style *s = it->node ? it->node->style : 0;
    if (!s || !s->tsh.a || it->font == FONT_MONO)
        return;
    int a = s->tsh.a, bl = s->tsh.blur;
    if (bl)                                              /* rozmycie: kilka slabszych kopii dookola */
        a = a * 2 / 5;
    gfx_text_alpha(a);
    font_draw(it->font, x + s->tsh.x, y + s->tsh.y, it->text, it->len, s->tsh.color);
    if (bl > 1) {
        int d = bl / 2 > 3 ? 3 : bl / 2;
        gfx_text_alpha(a / 2);
        for (int k = 0; k < 4; k++)
            font_draw(it->font, x + s->tsh.x + (k == 0 ? -d : k == 1 ? d : 0),
                      y + s->tsh.y + (k == 2 ? -d : k == 3 ? d : 0), it->text, it->len, s->tsh.color);
    }
    gfx_text_alpha(255);
}

#endif

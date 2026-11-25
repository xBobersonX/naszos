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

/* Przyciecie (overflow): element i to DL_CLIP. Zapamietuje stary prostokat
 * obcinania, ustawia czesc wspolna z oknem elementu i mowi, o ile przesunac
 * w gore jego wnetrze (przewiniecie). Pasek przewijania rysuje paint_clip_end. */
#define PAINT_CLIPS 8
#define CLIP_BAR_W 10

struct paint_clip {
    unsigned end;
    struct rect saved;
    int x, y, w, h;                      /* okno elementu na ekranie */
    int dy;                              /* przewiniecie wnetrza */
    const struct dl_item *it;
};

static inline void paint_clip_begin(const struct page_layout *L, unsigned i, int ox, int oy, struct paint_clip *c)
{
    const struct dl_item *it = &L->items[i];
    c->end = i + 1 + (unsigned)it->link;
    if (c->end > L->n)
        c->end = L->n;
    c->it = it;
    c->x = ox + it->x;
    c->y = oy + it->y;
    c->w = it->w;
    c->h = it->h;
    c->dy = it->node ? it->node->scroll_y : 0;
    c->saved = gfx_get_clip();
    struct rect r = c->saved;
    int x1 = r.x > c->x ? r.x : c->x, y1 = r.y > c->y ? r.y : c->y;
    int x2 = r.x + r.w < c->x + c->w ? r.x + r.w : c->x + c->w;
    int y2 = r.y + r.h < c->y + c->h ? r.y + r.h : c->y + c->h;
    struct rect n = { x1, y1, x2 > x1 ? x2 - x1 : 0, y2 > y1 ? y2 - y1 : 0 };
    gfx_set_clip(&n);
}

/* Czy element ma pasek przewijania (auto: gdy tresc sie nie miesci, scroll: zawsze) */
static inline int clip_has_bar(const struct dl_item *it)
{
    return it->node && (it->ext[0] == 3 || (it->ext[0] == 2 && it->node->scroll_max > 0));
}

static inline void paint_clip_end(struct paint_clip *c)
{
    const struct dl_item *it = c->it;
    if (clip_has_bar(it) && c->w > CLIP_BAR_W * 2 && c->h > 8) {    /* pasek w prawej czesci okna */
        int bx = c->x + c->w - CLIP_BAR_W, max = it->node->scroll_max;
        gfx_fill(bx, c->y, CLIP_BAR_W, c->h, 0xF1F1F1);
        if (max > 0) {
            int total = c->h + max, thumb = c->h * c->h / total;
            if (thumb < 16)
                thumb = 16;
            int ty = c->y + (c->h - thumb) * c->dy / max;
            gfx_fill(bx + 2, ty, CLIP_BAR_W - 4, thumb, 0xA8A8A8);
        }
    }
    gfx_set_clip(&c->saved);
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

/* Sprzatanie pamieci JavaScriptu (GC) - "ostrozne" zaznacz-i-zamiec.
 *
 * Kazda alokacja silnika JS (js_alloc) trafia tutaj. Male bloki (do 2 KB)
 * leza na stronach po 4 KB, a jedna strona trzyma bloki jednego rozmiaru
 * (16, 32, 48 ... 2048). Strony sa pociete z segmentow po 1 MB. Wieksze
 * bloki dostaja wlasny kawalek z kmalloc.
 *
 * Sprzatanie (js_gc_collect):
 *  1. ZAZNACZ: zaczynamy od korzeni (struktura rt, stosy, rejestry, drzewo
 *     DOM). Kazde 8 bajtow, ktore WYGLADA jak adres naszego bloku, uznajemy
 *     za wskaznik ("ostroznie" - nie wiemy, co jest czym). Taki blok jest
 *     zywy i przegladamy z kolei jego wnetrze.
 *  2. ZAMIEC: bloki niezaznaczone sa wolne i wracaja do ponownego uzycia.
 *
 * Rodzaje blokow:
 *  GK_SCAN - moze zawierac wskazniki (obiekty, srodowiska, AST...),
 *  GK_RAW  - same bajty (znaki, piksele, liczby) - nie przegladamy,
 *  GK_STR  - tekst jstr: przegladamy tylko naglowek, znaki leza za nim. */

#include "js_int.h"
#include "heap.h"
#include "string.h"
#include "dom.h"

#define GPAGE     4096
#define SEG_PAGES 256                     /* segment = 1 MB */
#define SMALL_MAX 2048
#define NCLS      22
#define MSTACK    65536                   /* wpisow na stosie zaznaczania */
#define MAX_STACKS 2048                   /* korutyny + generatory (400 + 1000) z zapasem */
#define MIN_TRIGGER (8u << 20)            /* sprzatamy najwczesniej po 8 MB nowych alokacji */

static const uint16_t cls_size[NCLS] = {
    16, 32, 48, 64, 80, 96, 112, 128, 160, 192, 224, 256,
    320, 384, 448, 512, 640, 768, 1024, 1360, 1632, 2048
};

struct gpage {
    uint8_t *mem;                         /* poczatek strony */
    struct gpage *next;                   /* lista stron z wolnymi blokami / wolnych stron */
    uint16_t size;                        /* rozmiar bloku, 0 = strona wolna */
    uint16_t n;                           /* ile blokow miesci */
    uint16_t pos;                         /* od ktorego bloku szukac wolnego */
    uint8_t kind, cls;
    uint64_t alloc[4], mark[4];           /* do 256 blokow na strone */
};

struct gseg {
    uint8_t *base, *end;                  /* zakres adresow */
    void *raw;                            /* to, co dal kmalloc */
    int big;                              /* 1 = jeden duzy blok */
    uint8_t kind, bmark;
    size_t bsize;
    struct gpage pg[];                    /* tylko dla zwyklych segmentow */
};

struct mentry {
    uintptr_t p;
    size_t n;
};

struct jgc {
    struct gseg **segs;                   /* posortowane po adresie */
    int nsegs, cap;
    uintptr_t lo, hi;                     /* zakres wszystkich segmentow - szybkie odrzucanie */
    struct gpage *cur[3][NCLS];           /* strony z wolnymi blokami */
    struct gpage *free_pages;
    size_t used;                          /* bajty w zajetych blokach */
    size_t since;                         /* przydzielone od ostatniego sprzatania */
    size_t live;                          /* zywe po ostatnim sprzataniu */
    size_t heap;                          /* razem wziete z kmalloc */
    size_t peak;                          /* najwiecej heap */
    struct mentry *ms;
    int msp, overflow;
    unsigned collections;
    unsigned torture, tcount;             /* sprzataj co N alokacji (testy) */
    int busy;
    uintptr_t sps[2 * MAX_STACKS + 1];    /* robocze: zapisane rsp i zakresy stosow */
    uint8_t *slo[MAX_STACKS + 1], *shi[MAX_STACKS + 1];
};

static uint8_t size_to_cls[SMALL_MAX / 16 + 1];

static struct jgc *gc_get(js_rt *rt)
{
    if (rt->gc)
        return rt->gc;
    if (!size_to_cls[1]) {
        int c = 0;
        for (int i = 0; i <= SMALL_MAX / 16; i++) {
            while (cls_size[c] < i * 16)
                c++;
            size_to_cls[i] = (uint8_t)c;
        }
    }
    struct jgc *g = kzalloc(sizeof(*g));
    if (!g)
        return 0;
    g->ms = kmalloc(sizeof(struct mentry) * MSTACK);
    g->lo = ~(uintptr_t)0;
    rt->gc = g;
    return g;
}

/* ---------- segmenty ---------- */

static int seg_insert(struct jgc *g, struct gseg *s)
{
    if (g->nsegs == g->cap) {
        int nc = g->cap ? g->cap * 2 : 64;
        struct gseg **ns = kmalloc(sizeof(*ns) * (size_t)nc);
        if (!ns)
            return 0;
        if (g->nsegs)
            memcpy(ns, g->segs, sizeof(*ns) * (size_t)g->nsegs);
        kfree(g->segs);
        g->segs = ns;
        g->cap = nc;
    }
    int i = g->nsegs;
    while (i > 0 && g->segs[i - 1]->base > s->base) {
        g->segs[i] = g->segs[i - 1];
        i--;
    }
    g->segs[i] = s;
    g->nsegs++;
    if ((uintptr_t)s->base < g->lo)
        g->lo = (uintptr_t)s->base;
    if ((uintptr_t)s->end > g->hi)
        g->hi = (uintptr_t)s->end;
    return 1;
}

static struct gseg *seg_find(struct jgc *g, uintptr_t a)
{
    int l = 0, r = g->nsegs - 1;
    while (l <= r) {
        int m = (l + r) >> 1;
        struct gseg *s = g->segs[m];
        if (a < (uintptr_t)s->base)
            r = m - 1;
        else if (a >= (uintptr_t)s->end)
            l = m + 1;
        else
            return s;
    }
    return 0;
}

static int new_segment(struct jgc *g)
{
    struct gseg *s = kzalloc(sizeof(*s) + sizeof(struct gpage) * SEG_PAGES);
    if (!s)
        return 0;
    s->raw = kmalloc((size_t)SEG_PAGES * GPAGE + GPAGE);
    if (!s->raw) {
        kfree(s);
        return 0;
    }
    s->base = (uint8_t *)(((uintptr_t)s->raw + GPAGE - 1) & ~(uintptr_t)(GPAGE - 1));
    s->end = s->base + (size_t)SEG_PAGES * GPAGE;
    if (!seg_insert(g, s)) {
        kfree(s->raw);
        kfree(s);
        return 0;
    }
    g->heap += (size_t)SEG_PAGES * GPAGE + GPAGE;
    for (int i = SEG_PAGES - 1; i >= 0; i--) {
        s->pg[i].mem = s->base + (size_t)i * GPAGE;
        s->pg[i].next = g->free_pages;
        g->free_pages = &s->pg[i];
    }
    return 1;
}

/* ---------- przydzial ---------- */

static void *alloc_big(struct jgc *g, size_t n, int kind)
{
    struct gseg *s = kzalloc(sizeof(*s));
    if (!s)
        return 0;
    n = (n + 15) & ~(size_t)15;
    s->raw = kmalloc(n);
    if (!s->raw) {
        kfree(s);
        return 0;
    }
    s->base = s->raw;
    s->end = s->base + n;
    s->big = 1;
    s->kind = (uint8_t)kind;
    s->bsize = n;
    if (!seg_insert(g, s)) {
        kfree(s->raw);
        kfree(s);
        return 0;
    }
    g->heap += n;
    g->used += n;
    g->since += n;
    memset(s->base, 0, n);
    return s->base;
}

static void *alloc_small(struct jgc *g, size_t n, int kind)
{
    int c = size_to_cls[(n + 15) >> 4];
    struct gpage *p = g->cur[kind][c];
    for (;;) {
        for (; p; p = g->cur[kind][c] = p->next) {
            while (p->pos < p->n) {
                unsigned i = p->pos;
                uint64_t w = p->alloc[i >> 6];
                if (w == ~(uint64_t)0) {
                    p->pos = (uint16_t)((i | 63) + 1);
                    continue;
                }
                p->pos++;
                if (!(w & (1ull << (i & 63)))) {
                    p->alloc[i >> 6] = w | (1ull << (i & 63));
                    void *r = p->mem + (size_t)i * p->size;
                    memset(r, 0, p->size);
                    g->used += p->size;
                    g->since += p->size;
                    return r;
                }
            }
        }
        /* nowa strona */
        if (!g->free_pages && !new_segment(g))
            return 0;
        p = g->free_pages;
        g->free_pages = p->next;
        p->size = cls_size[c];
        p->n = (uint16_t)(GPAGE / p->size);
        p->pos = 0;
        p->kind = (uint8_t)kind;
        p->cls = (uint8_t)c;
        memset(p->alloc, 0, sizeof(p->alloc));
        memset(p->mark, 0, sizeof(p->mark));
        p->next = 0;
        g->cur[kind][c] = p;
    }
}

/* ---------- zaznaczanie ---------- */

static size_t scan_len(int kind, size_t size)
{
    if (kind == GK_RAW)
        return 0;
    if (kind == GK_STR)
        return size < sizeof(jstr) ? size : sizeof(jstr);
    return size;
}

static void push(struct jgc *g, uintptr_t p, size_t n)
{
    if (!n)
        return;
    if (g->msp == MSTACK || !g->ms) {
        g->overflow = 1;                 /* przejrzymy go jeszcze raz pozniej */
        return;
    }
    g->ms[g->msp].p = p;
    g->ms[g->msp].n = n;
    g->msp++;
}

static void mark_ptr(struct jgc *g, uintptr_t a)
{
    struct gseg *s = seg_find(g, a);
    if (!s)
        return;
    if (s->big) {
        if (s->bmark)
            return;
        s->bmark = 1;
        push(g, (uintptr_t)s->base, scan_len(s->kind, s->bsize));
        return;
    }
    struct gpage *p = &s->pg[(a - (uintptr_t)s->base) / GPAGE];
    if (!p->size)
        return;
    unsigned i = (unsigned)((a - (uintptr_t)p->mem) / p->size);
    if (i >= p->n)
        return;
    uint64_t bit = 1ull << (i & 63);
    if (!(p->alloc[i >> 6] & bit) || (p->mark[i >> 6] & bit))
        return;
    p->mark[i >> 6] |= bit;
    push(g, (uintptr_t)p->mem + (size_t)i * p->size, scan_len(p->kind, p->size));
}

static void scan_range(struct jgc *g, const void *lo, const void *hi)
{
    uintptr_t a = ((uintptr_t)lo + 7) & ~(uintptr_t)7;
    uintptr_t glo = g->lo, ghi = g->hi;
    for (; a + 8 <= (uintptr_t)hi; a += 8) {
        uintptr_t v = *(const uintptr_t *)a;
        if (v >= glo && v < ghi)
            mark_ptr(g, v);
    }
}

static void drain(struct jgc *g)
{
    for (;;) {
        while (g->msp) {
            struct mentry e = g->ms[--g->msp];
            scan_range(g, (void *)e.p, (void *)(e.p + e.n));
        }
        if (!g->overflow)
            return;
        /* stos zaznaczania sie przepelnil: przegladamy jeszcze raz wszystkie zaznaczone */
        g->overflow = 0;
        for (int k = 0; k < g->nsegs; k++) {
            struct gseg *s = g->segs[k];
            if (s->big) {
                if (s->bmark)
                    push(g, (uintptr_t)s->base, scan_len(s->kind, s->bsize));
                continue;
            }
            for (int j = 0; j < SEG_PAGES; j++) {
                struct gpage *p = &s->pg[j];
                if (!p->size || p->kind == GK_RAW)
                    continue;
                for (unsigned i = 0; i < p->n; i++)
                    if (p->mark[i >> 6] & (1ull << (i & 63)))
                        push(g, (uintptr_t)p->mem + (size_t)i * p->size, scan_len(p->kind, p->size));
            }
            if (g->msp > MSTACK / 2) {
                int ov = g->overflow;
                g->overflow = 0;
                while (g->msp) {
                    struct mentry e = g->ms[--g->msp];
                    scan_range(g, (void *)e.p, (void *)(e.p + e.n));
                }
                g->overflow |= ov;
            }
        }
    }
}

/* ---------- zamiatanie ---------- */

static void sweep(struct jgc *g)
{
    memset(g->cur, 0, sizeof(g->cur));
    g->free_pages = 0;
    size_t used = 0;
    int out = 0, empty = 0;
    for (int k = 0; k < g->nsegs; k++) {
        struct gseg *s = g->segs[k];
        if (s->big) {
            if (!s->bmark) {                 /* duzy blok nieuzywany - oddajemy od razu */
                g->heap -= s->bsize;
                kfree(s->raw);
                kfree(s);
                continue;
            }
            s->bmark = 0;
            used += s->bsize;
            g->segs[out++] = s;
            continue;
        }
        struct gpage *free_before = g->free_pages;
        int seg_live = 0;
        for (int j = SEG_PAGES - 1; j >= 0; j--) {
            struct gpage *p = &s->pg[j];
            if (!p->size) {
                p->next = g->free_pages;
                g->free_pages = p;
                continue;
            }
            unsigned live = 0;
            for (int w = 0; w < 4; w++) {
                p->alloc[w] &= p->mark[w];
                p->mark[w] = 0;
                live += (unsigned)__builtin_popcountll(p->alloc[w]);
            }
            if (!live) {
                p->size = 0;
                p->next = g->free_pages;
                g->free_pages = p;
                continue;
            }
            used += (size_t)live * p->size;
            seg_live = 1;
            if (live < p->n) {
                p->pos = 0;
                p->next = g->cur[p->kind][p->cls];
                g->cur[p->kind][p->cls] = p;
            }
        }
        /* Caly segment pusty: zostawiamy 2 w zapasie, reszte oddajemy do kmalloc
         * (jego strony leza na samej gorze listy wolnych - cofamy je). */
        if (!seg_live && ++empty > 2) {
            g->free_pages = free_before;
            g->heap -= (size_t)SEG_PAGES * GPAGE + GPAGE;
            kfree(s->raw);
            kfree(s);
            continue;
        }
        g->segs[out++] = s;
    }
    g->nsegs = out;
    g->lo = ~(uintptr_t)0;
    g->hi = 0;
    for (int k = 0; k < out; k++) {
        if ((uintptr_t)g->segs[k]->base < g->lo)
            g->lo = (uintptr_t)g->segs[k]->base;
        if ((uintptr_t)g->segs[k]->end > g->hi)
            g->hi = (uintptr_t)g->segs[k]->end;
    }
    g->used = used;
}

/* ---------- korzenie ---------- */

static void scan_arena_cb(void *ctx, const void *lo, const void *hi)
{
    scan_range(ctx, lo, hi);
}

/* Zrzut rejestrow na stos - wskaznik moze siedziec tylko w rejestrze */
__attribute__((noinline)) static void scan_registers_and_stacks(js_rt *rt, struct jgc *g)
{
    uint64_t regs[8];
    __asm__ volatile(
        "mov %%rbx, 0(%0)\n\t"
        "mov %%rbp, 8(%0)\n\t"
        "mov %%r12, 16(%0)\n\t"
        "mov %%r13, 24(%0)\n\t"
        "mov %%r14, 32(%0)\n\t"
        "mov %%r15, 40(%0)\n\t"
        "mov %%rsi, 48(%0)\n\t"
        "mov %%rdi, 56(%0)\n\t"
        :: "r"(regs) : "memory");
    scan_range(g, regs, regs + 8);

    /* Stosy: glowny stos JS i stosy korutyn. Dla kazdego szukamy najnizszego
     * zywego adresu: biezacy rsp albo rsp zapisany przy przelaczeniu. */
    uintptr_t *sp_list = g->sps;
    int nsp = 0;
    sp_list[nsp++] = (uintptr_t)regs;
    nsp += js_coro_sps(rt, sp_list + nsp, 2 * MAX_STACKS);

    uint8_t **lo = g->slo, **hi = g->shi;
    int nst = 0;
    if (rt->stack) {
        lo[nst] = rt->stack;
        hi[nst++] = rt->stack + JS_STACK_SIZE;
    }
    nst += js_coro_stacks(rt, lo + nst, hi + nst, MAX_STACKS);
    for (int i = 0; i < nst; i++) {
        uintptr_t low = (uintptr_t)hi[i];
        for (int k = 0; k < nsp; k++)
            if (sp_list[k] >= (uintptr_t)lo[i] && sp_list[k] < (uintptr_t)hi[i] && sp_list[k] < low)
                low = sp_list[k];
        if (low == (uintptr_t)hi[i])
            low = (uintptr_t)lo[i];          /* nie wiemy, gdzie szczyt - caly stos (bezpiecznie) */
        scan_range(g, (void *)low, hi[i]);
    }
}

void js_gc_collect(js_rt *rt)
{
    struct jgc *g = rt->gc;
    if (!g || g->busy || !rt->on_stack)
        return;
    g->busy = 1;
    g->msp = 0;
    g->overflow = 0;
    scan_range(g, rt, rt + 1);
    scan_registers_and_stacks(rt, g);
    if (rt->doc)                             /* wezly DOM (node->js) leza w arenie dokumentu */
        arena_scan(&rt->doc->mem, scan_arena_cb, g);
    drain(g);
    sweep(g);
    g->collections++;
    g->since = 0;
    g->live = g->used;
    g->busy = 0;
}

/* ---------- interfejs ---------- */

void *js_alloc_kind(js_rt *rt, size_t n, int kind)
{
    struct jgc *g = gc_get(rt);
    if (!g)
        return 0;
    if (rt->on_stack && !rt->aborting && !g->busy) {
        size_t trig = g->live > MIN_TRIGGER ? g->live : MIN_TRIGGER;   /* rosniemy najwyzej 2x zywych */
        if (g->since >= trig || g->used + n > rt->mem_limit ||
            (g->torture && ++g->tcount >= g->torture)) {
            g->tcount = 0;
            js_gc_collect(rt);
        }
    }
    void *p = n > SMALL_MAX ? alloc_big(g, n, kind) : alloc_small(g, n ? n : 1, kind);
    if (g->heap > g->peak)
        g->peak = g->heap;
    if ((!p || g->used > rt->mem_limit) && rt->catch_top && !rt->aborting) {
        rt->dead = 1;
        js_abort(rt, "Brak pamieci dla JavaScriptu - skrypt przerwany");
    }
    return p;
}

void js_gc_free_all(js_rt *rt)
{
    struct jgc *g = rt->gc;
    if (!g)
        return;
    for (int k = 0; k < g->nsegs; k++) {
        kfree(g->segs[k]->raw);
        kfree(g->segs[k]);
    }
    kfree(g->segs);
    kfree(g->ms);
    kfree(g);
    rt->gc = 0;
}

void js_gc_set_torture(js_rt *rt, unsigned every)
{
    struct jgc *g = gc_get(rt);
    if (g)
        g->torture = every;
}

void js_gc_info(js_rt *rt, unsigned *used_kb, unsigned *peak_kb, unsigned *collections)
{
    struct jgc *g = rt ? rt->gc : 0;
    *used_kb = g ? (unsigned)(g->used / 1024) : 0;
    *peak_kb = g ? (unsigned)(g->peak / 1024) : 0;
    *collections = g ? g->collections : 0;
}

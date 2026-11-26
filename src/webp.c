/* Dekoder WebP - patrz webp.h.
 *
 * Plik WebP to "pudelko" RIFF z kawalkami (chunk): naglowek 4 litery + dlugosc.
 *
 *  VP8L - bezstratny. Obraz to ciag pikseli ARGB zakodowany kodami Huffmana
 *         (jak w PNG/ZIP), z "kopiuj N pikseli sprzed M pikseli" i mala
 *         pamiecia ostatnich kolorow (color cache). Przed kodowaniem obraz
 *         moze byc przeksztalcony: przewidywanie z sasiadow, odjecie zielonego,
 *         korelacja kolorow albo paleta - odwracamy to w odwrotnej kolejnosci.
 *  VP8  - stratny (klatka kluczowa z filmow WebM). Obraz w YUV podzielony na
 *         makrobloki 16x16; kazdy blok jest "zgadywany" z pikseli nad i obok
 *         (intra prediction), a do zgadnietego dodajemy poprawke zapisana jako
 *         fale (DCT 4x4, liczby calkowite). Bity czyta dekoder arytmetyczny
 *         ("boolean decoder") z prawdopodobienstwami z tablic. Na koniec filtr
 *         wygladza krawedzie miedzy blokami.
 *  ALPH - przezroczystosc do VP8: bajty alfa surowe albo jako obraz VP8L
 *         (alfa w kanale zielonym), z prostym filtrem (roznica z sasiadem).
 * Wszystko na liczbach calkowitych (jadro nie uzywa float). */

#include "webp.h"
#include "heap.h"
#include "string.h"
#include "thread.h"
#include "webp_tab.h"

#define BREATHE() thread_yield()
#define MAX_PIXELS (4u * 1024 * 1024)
#define DIV(a, b) (((a) + (1 << (b)) - 1) >> (b))

static inline unsigned le24(const uint8_t *p) { return p[0] | p[1] << 8 | (unsigned)p[2] << 16; }
static inline unsigned le32(const uint8_t *p) { return le24(p) | (unsigned)p[3] << 24; }
static inline int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* ============================ VP8L (bezstratny) ============================ */

/* Bity czytane od najmlodszego */
struct lbr {
    const uint8_t *p, *end;
    uint64_t val;
    int n;          /* ile bitow w val */
    int over;       /* ile bajtow "dopisalismy" za koncem danych */
};

static void lfill(struct lbr *b)
{
    while (b->n <= 56) {
        uint64_t c = 0;
        if (b->p < b->end)
            c = *b->p++;
        else
            b->over++;
        b->val |= c << b->n;
        b->n += 8;
    }
}

static uint32_t lbits(struct lbr *b, int k)
{
    if (b->n < k)
        lfill(b);
    uint32_t v = (uint32_t)(b->val & ((1ull << k) - 1));
    b->val >>= k;
    b->n -= k;
    return v;
}

/* Kod Huffmana: szybka tablica na 8 bitow, dluzsze kody bit po bicie */
struct huff {
    uint16_t fast[256];     /* symbol | dlugosc << 12; 0 = kod dluzszy niz 8 */
    uint16_t count[16];     /* ile symboli ma dana dlugosc kodu */
    uint16_t *sym;          /* symbole posortowane wg dlugosci kodu */
    int single;             /* >= 0: jedyny symbol, kod bez bitow */
};

static void huff_free(struct huff *h)
{
    kfree(h->sym);
    h->sym = 0;
}

static int huff_build(struct huff *h, const uint8_t *len, int n)
{
    int nz = 0, last = 0;
    memset(h->count, 0, sizeof h->count);
    memset(h->fast, 0, sizeof h->fast);
    h->sym = 0;
    h->single = -1;
    for (int i = 0; i < n; i++)
        if (len[i]) {
            h->count[len[i]]++;
            nz++;
            last = i;
        }
    if (nz <= 1) {
        h->single = last;
        return 0;
    }
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left = (left << 1) - h->count[l];
        if (left < 0)
            return IMG_ECORRUPT;        /* za duzo kodow */
    }
    h->sym = kmalloc((size_t)nz * 2);
    if (!h->sym)
        return IMG_ENOMEM;
    uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++)
        offs[l + 1] = (uint16_t)(offs[l] + h->count[l]);
    for (int i = 0; i < n; i++)
        if (len[i])
            h->sym[offs[len[i]]++] = (uint16_t)i;
    /* kody kanoniczne; w strumieniu najstarszy bit kodu idzie pierwszy */
    int code = 0, k = 0;
    for (int l = 1; l <= 8; l++, code <<= 1)
        for (int c = 0; c < h->count[l]; c++, k++, code++) {
            int r = 0;
            for (int i = 0; i < l; i++)
                r |= (code >> i & 1) << (l - 1 - i);
            for (int f = r; f < 256; f += 1 << l)
                h->fast[f] = (uint16_t)(h->sym[k] | l << 12);
        }
    return 0;
}

static int hdec(struct lbr *b, const struct huff *h)
{
    if (h->single >= 0)
        return h->single;
    if (b->n < 16)
        lfill(b);
    unsigned e = h->fast[b->val & 255];
    if (e) {
        int l = (int)(e >> 12);
        b->val >>= l;
        b->n -= l;
        return (int)(e & 0xfff);
    }
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        code |= (int)(b->val & 1);
        b->val >>= 1;
        b->n--;
        int c = h->count[l];
        if (code - c < first)
            return h->sym[index + code - first];
        index += c;
        first = (first + c) << 1;
        code <<= 1;
    }
    return -1;
}

static int read_code(struct lbr *b, struct huff *h, int n, uint8_t *lens)
{
    memset(lens, 0, (size_t)n);
    if (lbits(b, 1)) {                          /* "prosty" kod: 1 albo 2 symbole */
        int ns = (int)lbits(b, 1) + 1;
        int s = (int)lbits(b, lbits(b, 1) ? 8 : 1);
        if (s >= n)
            return IMG_ECORRUPT;
        lens[s] = 1;
        if (ns == 2) {
            s = (int)lbits(b, 8);
            if (s >= n)
                return IMG_ECORRUPT;
            lens[s] = 1;
        }
        return huff_build(h, lens, n);
    }
    static const uint8_t order[19] = { 17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    uint8_t cl[19] = { 0 };
    int nc = (int)lbits(b, 4) + 4;
    for (int i = 0; i < nc; i++)
        cl[order[i]] = (uint8_t)lbits(b, 3);
    struct huff lh;
    int err = huff_build(&lh, cl, 19);
    if (err)
        return err;
    int max = n;
    if (lbits(b, 1)) {
        int nb = 2 + 2 * (int)lbits(b, 3);
        max = 2 + (int)lbits(b, nb);
        if (max > n)
            goto bad;
    }
    int s = 0, prev = 8;
    while (s < n) {
        if (max-- == 0)
            break;
        int c = hdec(b, &lh);
        if (c < 0)
            goto bad;
        if (c < 16) {
            lens[s++] = (uint8_t)c;
            if (c)
                prev = c;
        } else {
            int rep = c == 16 ? 3 + (int)lbits(b, 2) : c == 17 ? 3 + (int)lbits(b, 3) : 11 + (int)lbits(b, 7);
            if (s + rep > n)
                goto bad;
            int v = c == 16 ? prev : 0;
            while (rep--)
                lens[s++] = (uint8_t)v;
        }
    }
    huff_free(&lh);
    return huff_build(h, lens, n);
bad:
    huff_free(&lh);
    return IMG_ECORRUPT;
}

/* Dlugosci i odleglosci: symbol + dodatkowe bity */
static int prefix_val(struct lbr *b, int s)
{
    if (s < 4)
        return s + 1;
    int extra = (s - 2) >> 1;
    return ((2 + (s & 1)) << extra) + (int)lbits(b, extra) + 1;
}

#define MAX_GROUPS 8192

/* Obraz zakodowany entropijnie. main = 1 dla glownego obrazu (moze miec
 * "meta" obraz z numerami grup kodow dla kawalkow obrazu) */
static int decode_level(struct lbr *b, uint32_t *out, int w, int h, int main)
{
    int cbits = 0, err = 0;
    if (lbits(b, 1)) {
        cbits = (int)lbits(b, 4);
        if (cbits < 1 || cbits > 11)
            return IMG_ECORRUPT;
    }
    uint32_t *meta = 0, *cache = 0;
    struct huff *groups = 0;
    uint8_t *lens = 0;
    int mbits = 0, mw = 0, ng = 1;
    if (main && lbits(b, 1)) {
        mbits = (int)lbits(b, 3) + 2;
        mw = DIV(w, mbits);
        int mh = DIV(h, mbits);
        meta = kmalloc((size_t)mw * (size_t)mh * 4);
        if (!meta)
            return IMG_ENOMEM;
        if ((err = decode_level(b, meta, mw, mh, 0)))
            goto out;
        for (int i = 0; i < mw * mh; i++) {
            meta[i] = meta[i] >> 8 & 0xffff;
            if ((int)meta[i] >= ng)
                ng = (int)meta[i] + 1;
        }
        if (ng > MAX_GROUPS) {
            err = IMG_ETOOBIG;
            goto out;
        }
    }
    groups = kzalloc((size_t)ng * 5 * sizeof(struct huff));
    lens = kmalloc(280 + 2048);
    if (cbits)
        cache = kzalloc(4u << cbits);
    if (!groups || !lens || (cbits && !cache)) {
        err = IMG_ENOMEM;
        goto out;
    }
    for (int g = 0; g < ng * 5; g++)
        groups[g].single = 0;
    for (int g = 0; g < ng; g++)
        for (int k = 0; k < 5; k++) {
            int n = k == 0 ? 280 + (cbits ? 1 << cbits : 0) : k == 4 ? 40 : 256;
            if ((err = read_code(b, &groups[g * 5 + k], n, lens)))
                goto out;
        }

    int total = w * h, pos = 0, x = 0, y = 0;
    const struct huff *g = groups;
    while (pos < total) {
        if (meta)
            g = groups + 5 * meta[(y >> mbits) * mw + (x >> mbits)];
        if (b->over > 8) {
            err = IMG_ECORRUPT;
            goto out;
        }
        int s = hdec(b, &g[0]);
        if (s < 0) {
            err = IMG_ECORRUPT;
            goto out;
        }
        if (s < 256) {
            int r = hdec(b, &g[1]), bl = hdec(b, &g[2]), a = hdec(b, &g[3]);
            if ((r | bl | a) < 0) {
                err = IMG_ECORRUPT;
                goto out;
            }
            uint32_t p = (uint32_t)a << 24 | (uint32_t)r << 16 | (uint32_t)s << 8 | (uint32_t)bl;
            out[pos++] = p;
            if (cache)
                cache[(0x1e35a7bdu * p) >> (32 - cbits)] = p;
            if (++x == w) {
                x = 0;
                if ((++y & 31) == 0)
                    BREATHE();
            }
        } else if (s < 280) {
            int len = prefix_val(b, s - 256);
            int ds = hdec(b, &g[4]);
            if (ds < 0) {
                err = IMG_ECORRUPT;
                goto out;
            }
            int d = prefix_val(b, ds);
            if (d > 120)
                d -= 120;
            else {
                d = kDistMap[d - 1][0] + kDistMap[d - 1][1] * w;
                if (d < 1)
                    d = 1;
            }
            if (d > pos || len > total - pos) {
                err = IMG_ECORRUPT;
                goto out;
            }
            for (int i = 0; i < len; i++) {
                uint32_t p = out[pos - d];
                out[pos++] = p;
                if (cache)
                    cache[(0x1e35a7bdu * p) >> (32 - cbits)] = p;
            }
            x += len;
            while (x >= w) {
                x -= w;
                if ((++y & 31) == 0)
                    BREATHE();
            }
        } else {
            int i = s - 280;
            if (!cache || i >= 1 << cbits) {
                err = IMG_ECORRUPT;
                goto out;
            }
            out[pos++] = cache[i];
            if (++x == w) {
                x = 0;
                y++;
            }
        }
    }
out:
    if (groups)
        for (int i = 0; i < ng * 5; i++)
            huff_free(&groups[i]);
    kfree(groups);
    kfree(lens);
    kfree(cache);
    kfree(meta);
    return err;
}

/* Dodawanie skladowych ARGB osobno (modulo 256) */
static inline uint32_t add_px(uint32_t a, uint32_t b)
{
    return (((a & 0xff00ff00u) + (b & 0xff00ff00u)) & 0xff00ff00u) |
           (((a & 0x00ff00ffu) + (b & 0x00ff00ffu)) & 0x00ff00ffu);
}
static inline uint32_t avg2(uint32_t a, uint32_t b)
{
    return (((a ^ b) & 0xfefefefeu) >> 1) + (a & b);
}
static uint32_t select_px(uint32_t L, uint32_t T, uint32_t TL)
{
    int pl = 0, pt = 0;
    for (int s = 0; s < 32; s += 8) {
        int l = (int)(L >> s & 255), t = (int)(T >> s & 255), tl = (int)(TL >> s & 255);
        pl += t > tl ? t - tl : tl - t;
        pt += l > tl ? l - tl : tl - l;
    }
    return pl < pt ? L : T;
}
static uint32_t clamp_full(uint32_t a, uint32_t b, uint32_t c)
{
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8)
        r |= (uint32_t)clamp255((int)(a >> s & 255) + (int)(b >> s & 255) - (int)(c >> s & 255)) << s;
    return r;
}
static uint32_t clamp_half(uint32_t a, uint32_t b)
{
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        int x = (int)(a >> s & 255), y = (int)(b >> s & 255);
        r |= (uint32_t)clamp255(x + (x - y) / 2) << s;
    }
    return r;
}

static void inv_predict(uint32_t *d, int w, int h, int bits, const uint32_t *m)
{
    int bw = DIV(w, bits);
    d[0] = add_px(d[0], 0xff000000u);
    for (int x = 1; x < w; x++)
        d[x] = add_px(d[x], d[x - 1]);
    for (int y = 1; y < h; y++) {
        uint32_t *p = d + (size_t)y * w;
        const uint32_t *mr = m + (y >> bits) * bw;
        p[0] = add_px(p[0], p[-w]);
        for (int x = 1; x < w; x++) {
            uint32_t L = p[x - 1], T = p[x - w], TR = p[x - w + 1], TL = p[x - w - 1], pr;
            switch (mr[x >> bits] >> 8 & 15) {
            case 1:  pr = L; break;
            case 2:  pr = T; break;
            case 3:  pr = TR; break;
            case 4:  pr = TL; break;
            case 5:  pr = avg2(avg2(L, TR), T); break;
            case 6:  pr = avg2(L, TL); break;
            case 7:  pr = avg2(L, T); break;
            case 8:  pr = avg2(TL, T); break;
            case 9:  pr = avg2(T, TR); break;
            case 10: pr = avg2(avg2(L, TL), avg2(T, TR)); break;
            case 11: pr = select_px(L, T, TL); break;
            case 12: pr = clamp_full(L, T, TL); break;
            case 13: pr = clamp_half(avg2(L, T), TL); break;
            default: pr = 0xff000000u; break;
            }
            p[x] = add_px(p[x], pr);
        }
        if ((y & 63) == 0)
            BREATHE();
    }
}

static inline int ctd(int t, int c) { return ((int)(int8_t)t * (int)(int8_t)c) >> 5; }

static void inv_color(uint32_t *d, int w, int h, int bits, const uint32_t *m)
{
    int bw = DIV(w, bits);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t e = m[(y >> bits) * bw + (x >> bits)], p = d[(size_t)y * w + x];
            int g = (int)(p >> 8 & 255), r = (int)(p >> 16 & 255), bl = (int)(p & 255);
            r = (r + ctd((int)(e & 255), g)) & 255;
            bl = (bl + ctd((int)(e >> 8 & 255), g)) & 255;
            bl = (bl + ctd((int)(e >> 16 & 255), r)) & 255;
            d[(size_t)y * w + x] = (p & 0xff00ff00u) | (uint32_t)r << 16 | (uint32_t)bl;
        }
}

/* Paleta: indeksy w kanale zielonym, przy malej palecie kilka na piksel */
static void inv_index(uint32_t *d, int w, int h, int bits, const uint32_t *pal)
{
    if (bits == 0) {
        for (size_t i = 0; i < (size_t)w * h; i++)
            d[i] = pal[d[i] >> 8 & 255];
        return;
    }
    int pw = DIV(w, bits), bpp = 8 >> bits, mask = (1 << bpp) - 1, per = (1 << bits) - 1;
    /* od konca, bo wynik jest szerszy niz dane (to samo miejsce w pamieci) */
    for (int y = h - 1; y >= 0; y--)
        for (int x = w - 1; x >= 0; x--) {
            int v = (int)(d[(size_t)y * pw + (x >> bits)] >> 8 & 255);
            d[(size_t)y * w + x] = pal[v >> ((x & per) * bpp) & mask];
        }
}

/* Strumien VP8L bez naglowka (tez alfa w ALPH): przeksztalcenia + obraz */
static int vp8l_stream(struct lbr *b, uint32_t *out, int w, int h)
{
    struct { int type, bits, xs; uint32_t *data; } t[4];
    int nt = 0, xs = w, used = 0, err = 0;
    while (lbits(b, 1)) {
        int ty = (int)lbits(b, 2);
        if (used & 1 << ty) {
            err = IMG_ECORRUPT;
            goto out;
        }
        used |= 1 << ty;
        t[nt].type = ty;
        t[nt].xs = xs;
        t[nt].bits = 0;
        t[nt].data = 0;
        nt++;
        if (ty == 0 || ty == 1) {
            int bits = (int)lbits(b, 3) + 2, bw = DIV(xs, bits), bh = DIV(h, bits);
            t[nt - 1].bits = bits;
            t[nt - 1].data = kmalloc((size_t)bw * bh * 4);
            if (!t[nt - 1].data) {
                err = IMG_ENOMEM;
                goto out;
            }
            if ((err = decode_level(b, t[nt - 1].data, bw, bh, 0)))
                goto out;
        } else if (ty == 3) {
            int n = (int)lbits(b, 8) + 1;
            int bits = n > 16 ? 0 : n > 4 ? 1 : n > 2 ? 2 : 3;
            uint32_t *pal = kzalloc(256 * 4);
            t[nt - 1].bits = bits;
            t[nt - 1].data = pal;
            if (!pal) {
                err = IMG_ENOMEM;
                goto out;
            }
            if ((err = decode_level(b, pal, n, 1, 0)))
                goto out;
            for (int i = 1; i < n; i++)
                pal[i] = add_px(pal[i], pal[i - 1]);
            xs = DIV(xs, bits);
        }
    }
    if ((err = decode_level(b, out, xs, h, 1)))
        goto out;
    for (int i = nt - 1; i >= 0; i--) {
        int W = t[i].xs;
        switch (t[i].type) {
        case 0: inv_predict(out, W, h, t[i].bits, t[i].data); break;
        case 1: inv_color(out, W, h, t[i].bits, t[i].data); break;
        case 2:
            for (size_t k = 0; k < (size_t)W * h; k++) {
                uint32_t p = out[k], g = p >> 8 & 255;
                out[k] = (p & 0xff00ff00u) | ((p + (g << 16)) & 0xff0000u) | ((p + g) & 0xffu);
            }
            break;
        case 3: inv_index(out, W, h, t[i].bits, t[i].data); break;
        }
    }
out:
    for (int i = 0; i < nt; i++)
        kfree(t[i].data);
    return err;
}

static int vp8l_decode(const uint8_t *d, unsigned len, uint32_t **px, int *pw, int *ph)
{
    if (len < 5 || d[0] != 0x2f)
        return IMG_ECORRUPT;
    struct lbr b = { d + 1, d + len, 0, 0, 0 };
    int w = (int)lbits(&b, 14) + 1, h = (int)lbits(&b, 14) + 1;
    lbits(&b, 1);                       /* "czy jest alfa" - i tak sprawdzamy piksele */
    if (lbits(&b, 3) != 0)
        return IMG_ECORRUPT;
    if ((uint64_t)w * h > MAX_PIXELS)
        return IMG_ETOOBIG;
    uint32_t *out = kmalloc((size_t)w * h * 4);
    if (!out)
        return IMG_ENOMEM;
    int err = vp8l_stream(&b, out, w, h);
    if (err) {
        kfree(out);
        return err;
    }
    *px = out;
    *pw = w;
    *ph = h;
    return 0;
}

/* ============================ ALPH ============================ */

static int alph_decode(const uint8_t *d, unsigned len, uint8_t *a, int w, int h)
{
    if (len < 1)
        return IMG_ECORRUPT;
    int comp = d[0] & 3, filt = d[0] >> 2 & 3;
    size_t n = (size_t)w * h;
    if (comp == 0) {
        if (len - 1 < n)
            return IMG_ECORRUPT;
        memcpy(a, d + 1, n);
    } else if (comp == 1) {
        uint32_t *tmp = kmalloc(n * 4);
        if (!tmp)
            return IMG_ENOMEM;
        struct lbr b = { d + 1, d + len, 0, 0, 0 };
        int err = vp8l_stream(&b, tmp, w, h);
        for (size_t i = 0; !err && i < n; i++)
            a[i] = (uint8_t)(tmp[i] >> 8);
        kfree(tmp);
        if (err)
            return err;
    } else
        return IMG_ECORRUPT;
    if (filt == 0)
        return 0;
    for (int y = 0; y < h; y++) {
        uint8_t *r = a + (size_t)y * w, *u = r - w;
        for (int x = 0; x < w; x++) {
            int pr;
            if (y == 0)
                pr = x ? r[x - 1] : 0;
            else if (x == 0)
                pr = u[0];
            else if (filt == 1)
                pr = r[x - 1];
            else if (filt == 2)
                pr = u[x];
            else
                pr = clamp255(r[x - 1] + u[x] - u[x - 1]);
            r[x] = (uint8_t)(r[x] + pr);
        }
    }
    return 0;
}

/* ============================ VP8 (stratny) ============================ */

struct bdec {                   /* dekoder arytmetyczny bitow (RFC 6386, rozdz. 7) */
    const uint8_t *p, *end;
    uint32_t value, range;
    int bits;
};

static void bd_init(struct bdec *b, const uint8_t *p, const uint8_t *end)
{
    b->p = p;
    b->end = end;
    b->value = 0;
    for (int i = 0; i < 2; i++)
        b->value = b->value << 8 | (b->p < b->end ? *b->p++ : 0);
    b->range = 255;
    b->bits = 0;
}

static int bget(struct bdec *b, int prob)
{
    uint32_t split = 1 + (((b->range - 1) * (uint32_t)prob) >> 8), big = split << 8;
    int r;
    if (b->value >= big) {
        r = 1;
        b->range -= split;
        b->value -= big;
    } else {
        r = 0;
        b->range = split;
    }
    while (b->range < 128) {
        b->value <<= 1;
        b->range <<= 1;
        if (++b->bits == 8) {
            b->bits = 0;
            if (b->p < b->end)
                b->value |= *b->p++;
        }
    }
    return r;
}

static int blit(struct bdec *b, int n)
{
    int v = 0;
    while (n--)
        v = v << 1 | bget(b, 128);
    return v;
}
static int bsigned(struct bdec *b, int n)
{
    int v = blit(b, n);
    return bget(b, 128) ? -v : v;
}
static int bopt(struct bdec *b, int n) { return bget(b, 128) ? bsigned(b, n) : 0; }

enum { M_DC, M_TM, M_VE, M_HE, M_RD, M_VR, M_LD, M_VL, M_HD, M_HU };

/* Drzewo trybow 4x4: >0 = nastepny wezel, <=0 = -tryb */
static const int8_t kYModes4[18] = {
    -M_DC, 1, -M_TM, 2, -M_VE, 3, 4, 6, -M_HE, 5, -M_RD, -M_VR, -M_LD, 7, -M_VL, 8, -M_HD, -M_HU
};
static const uint8_t kBands[17] = { 0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0 };
static const uint8_t kZigzag[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
static const uint8_t kCat3[] = { 173, 148, 140, 0 }, kCat4[] = { 176, 155, 140, 135, 0 },
                     kCat5[] = { 180, 157, 141, 134, 130, 0 },
                     kCat6[] = { 254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0 };
static const uint8_t *const kCat[4] = { kCat3, kCat4, kCat5, kCat6 };

typedef uint8_t probs_t[8][3][11];

static int large_value(struct bdec *b, const uint8_t *p)
{
    if (!bget(b, p[3]))
        return !bget(b, p[4]) ? 2 : 3 + bget(b, p[5]);
    if (!bget(b, p[6])) {
        if (!bget(b, p[7]))
            return 5 + bget(b, 159);
        int v = 7 + 2 * bget(b, 165);
        return v + bget(b, 145);
    }
    int b1 = bget(b, p[8]);
    int b0 = bget(b, p[9 + b1]);
    int cat = 2 * b1 + b0, v = 0;
    for (const uint8_t *t = kCat[cat]; *t; t++)
        v += v + bget(b, *t);
    return v + 3 + (8 << cat);
}

/* Wspolczynniki jednego bloku 4x4; zwraca pozycje za ostatnim niezerowym */
static int coeffs(struct bdec *b, const probs_t *pr, int ctx, int dcq, int acq, int n, int16_t *out)
{
    const uint8_t *p = (*pr)[kBands[n]][ctx];
    for (; n < 16; n++) {
        if (!bget(b, p[0]))
            return n;
        while (!bget(b, p[1])) {
            p = (*pr)[kBands[++n]][0];
            if (n == 16)
                return 16;
        }
        int v;
        if (!bget(b, p[2])) {
            v = 1;
            p = (*pr)[kBands[n + 1]][1];
        } else {
            v = large_value(b, p);
            p = (*pr)[kBands[n + 1]][2];
        }
        out[kZigzag[n]] = (int16_t)((bget(b, 128) ? -v : v) * (n > 0 ? acq : dcq));
    }
    return 16;
}

/* Odwrotna transformata Walsha-Hadamarda: 16 wartosci DC blokow Y */
static void wht(const int16_t *in, int16_t *out)
{
    int tmp[16];
    for (int i = 0; i < 4; i++) {
        int a0 = in[0 + i] + in[12 + i], a1 = in[4 + i] + in[8 + i];
        int a2 = in[4 + i] - in[8 + i], a3 = in[0 + i] - in[12 + i];
        tmp[0 + i] = a0 + a1;
        tmp[8 + i] = a0 - a1;
        tmp[4 + i] = a3 + a2;
        tmp[12 + i] = a3 - a2;
    }
    for (int i = 0; i < 4; i++) {
        int dc = tmp[0 + i * 4] + 3;
        int a0 = dc + tmp[3 + i * 4], a1 = tmp[1 + i * 4] + tmp[2 + i * 4];
        int a2 = tmp[1 + i * 4] - tmp[2 + i * 4], a3 = dc - tmp[3 + i * 4];
        out[0] = (int16_t)((a0 + a1) >> 3);
        out[16] = (int16_t)((a3 + a2) >> 3);
        out[32] = (int16_t)((a0 - a1) >> 3);
        out[48] = (int16_t)((a3 - a2) >> 3);
        out += 64;
    }
}

#define BPS 32                  /* szerokosc wiersza w buforze roboczym makrobloku */
#define MUL1(a) ((((a) * 20091) >> 16) + (a))
#define MUL2(a) (((a) * 35468) >> 16)

/* Odwrotna DCT 4x4 (calkowita) i dodanie do przewidzianych pikseli */
static void idct_add(const int16_t *in, uint8_t *dst)
{
    int i, any = 0;
    for (i = 0; i < 16; i++)
        any |= in[i];
    if (!any)
        return;
    int C[16], *t = C;
    for (i = 0; i < 4; i++, in++, t += 4) {
        int a = in[0] + in[8], b = in[0] - in[8];
        int c = MUL2(in[4]) - MUL1(in[12]), d = MUL1(in[4]) + MUL2(in[12]);
        t[0] = a + d;
        t[1] = b + c;
        t[2] = b - c;
        t[3] = a - d;
    }
    t = C;
    for (i = 0; i < 4; i++, t++, dst += BPS) {
        int dc = t[0] + 4;
        int a = dc + t[8], b = dc - t[8];
        int c = MUL2(t[4]) - MUL1(t[12]), d = MUL1(t[4]) + MUL2(t[12]);
        dst[0] = (uint8_t)clamp255(dst[0] + ((a + d) >> 3));
        dst[1] = (uint8_t)clamp255(dst[1] + ((b + c) >> 3));
        dst[2] = (uint8_t)clamp255(dst[2] + ((b - c) >> 3));
        dst[3] = (uint8_t)clamp255(dst[3] + ((a - d) >> 3));
    }
}

/* Przewidywanie calego bloku 16x16 albo 8x8 (chroma) */
static void pred_big(uint8_t *d, int n, int mode, int has_top, int has_left)
{
    int x, y, s = 0, sh = n == 16 ? 4 : 3;
    switch (mode) {
    case M_DC:
        if (has_top && has_left) {
            for (x = 0; x < n; x++)
                s += d[x - BPS] + d[x * BPS - 1];
            s = (s + n) >> (sh + 1);
        } else if (has_top || has_left) {
            for (x = 0; x < n; x++)
                s += has_top ? d[x - BPS] : d[x * BPS - 1];
            s = (s + n / 2) >> sh;
        } else
            s = 128;
        for (y = 0; y < n; y++)
            memset(d + y * BPS, s, (size_t)n);
        break;
    case M_TM:
        for (y = 0; y < n; y++)
            for (x = 0; x < n; x++)
                d[y * BPS + x] = (uint8_t)clamp255(d[y * BPS - 1] + d[x - BPS] - d[-BPS - 1]);
        break;
    case M_VE:
        for (y = 0; y < n; y++)
            memcpy(d + y * BPS, d - BPS, (size_t)n);
        break;
    default:            /* M_HE */
        for (y = 0; y < n; y++)
            memset(d + y * BPS, d[y * BPS - 1], (size_t)n);
        break;
    }
}

#define AVG3(a, b, c) ((uint8_t)(((a) + 2 * (b) + (c) + 2) >> 2))
#define AVG2(a, b) ((uint8_t)(((a) + (b) + 1) >> 1))
#define DST(x, y) d[(x) + (y) * BPS]

static void pred4(uint8_t *d, int mode)
{
    const uint8_t *t = d - BPS;
    int A = t[0], B = t[1], C = t[2], D = t[3], E = t[4], F = t[5], G = t[6], H = t[7];
    int X = t[-1], I = d[-1], J = d[BPS - 1], K = d[2 * BPS - 1], L = d[3 * BPS - 1];
    int x, y;
    switch (mode) {
    case M_DC: {
        int s = 4;
        for (x = 0; x < 4; x++)
            s += t[x] + d[x * BPS - 1];
        for (y = 0; y < 4; y++)
            memset(d + y * BPS, s >> 3, 4);
        break;
    }
    case M_TM:
        for (y = 0; y < 4; y++)
            for (x = 0; x < 4; x++)
                DST(x, y) = (uint8_t)clamp255(d[y * BPS - 1] + t[x] - X);
        break;
    case M_VE: {
        uint8_t v[4] = { AVG3(X, A, B), AVG3(A, B, C), AVG3(B, C, D), AVG3(C, D, E) };
        for (y = 0; y < 4; y++)
            memcpy(d + y * BPS, v, 4);
        break;
    }
    case M_HE:
        memset(d, AVG3(X, I, J), 4);
        memset(d + BPS, AVG3(I, J, K), 4);
        memset(d + 2 * BPS, AVG3(J, K, L), 4);
        memset(d + 3 * BPS, AVG3(K, L, L), 4);
        break;
    case M_RD:
        DST(0, 3) = AVG3(J, K, L);
        DST(1, 3) = DST(0, 2) = AVG3(I, J, K);
        DST(2, 3) = DST(1, 2) = DST(0, 1) = AVG3(X, I, J);
        DST(3, 3) = DST(2, 2) = DST(1, 1) = DST(0, 0) = AVG3(A, X, I);
        DST(3, 2) = DST(2, 1) = DST(1, 0) = AVG3(B, A, X);
        DST(3, 1) = DST(2, 0) = AVG3(C, B, A);
        DST(3, 0) = AVG3(D, C, B);
        break;
    case M_LD:
        DST(0, 0) = AVG3(A, B, C);
        DST(1, 0) = DST(0, 1) = AVG3(B, C, D);
        DST(2, 0) = DST(1, 1) = DST(0, 2) = AVG3(C, D, E);
        DST(3, 0) = DST(2, 1) = DST(1, 2) = DST(0, 3) = AVG3(D, E, F);
        DST(3, 1) = DST(2, 2) = DST(1, 3) = AVG3(E, F, G);
        DST(3, 2) = DST(2, 3) = AVG3(F, G, H);
        DST(3, 3) = AVG3(G, H, H);
        break;
    case M_VR:
        DST(0, 0) = DST(1, 2) = AVG2(X, A);
        DST(1, 0) = DST(2, 2) = AVG2(A, B);
        DST(2, 0) = DST(3, 2) = AVG2(B, C);
        DST(3, 0) = AVG2(C, D);
        DST(0, 3) = AVG3(K, J, I);
        DST(0, 2) = AVG3(J, I, X);
        DST(0, 1) = DST(1, 3) = AVG3(I, X, A);
        DST(1, 1) = DST(2, 3) = AVG3(X, A, B);
        DST(2, 1) = DST(3, 3) = AVG3(A, B, C);
        DST(3, 1) = AVG3(B, C, D);
        break;
    case M_VL:
        DST(0, 0) = AVG2(A, B);
        DST(1, 0) = DST(0, 2) = AVG2(B, C);
        DST(2, 0) = DST(1, 2) = AVG2(C, D);
        DST(3, 0) = DST(2, 2) = AVG2(D, E);
        DST(0, 1) = AVG3(A, B, C);
        DST(1, 1) = DST(0, 3) = AVG3(B, C, D);
        DST(2, 1) = DST(1, 3) = AVG3(C, D, E);
        DST(3, 1) = DST(2, 3) = AVG3(D, E, F);
        DST(3, 2) = AVG3(E, F, G);
        DST(3, 3) = AVG3(F, G, H);
        break;
    case M_HD:
        DST(0, 0) = DST(2, 1) = AVG2(I, X);
        DST(0, 1) = DST(2, 2) = AVG2(J, I);
        DST(0, 2) = DST(2, 3) = AVG2(K, J);
        DST(0, 3) = AVG2(L, K);
        DST(3, 0) = AVG3(A, B, C);
        DST(2, 0) = AVG3(X, A, B);
        DST(1, 0) = DST(3, 1) = AVG3(I, X, A);
        DST(1, 1) = DST(3, 2) = AVG3(J, I, X);
        DST(1, 2) = DST(3, 3) = AVG3(K, J, I);
        DST(1, 3) = AVG3(L, K, J);
        break;
    default:            /* M_HU */
        DST(0, 0) = AVG2(I, J);
        DST(2, 0) = DST(0, 1) = AVG2(J, K);
        DST(2, 1) = DST(0, 2) = AVG2(K, L);
        DST(1, 0) = AVG3(I, J, K);
        DST(3, 0) = DST(1, 1) = AVG3(J, K, L);
        DST(3, 1) = DST(1, 2) = AVG3(K, L, L);
        DST(3, 2) = DST(2, 2) = DST(0, 3) = DST(1, 3) = DST(2, 3) = DST(3, 3) = (uint8_t)L;
        break;
    }
}

/* ---- filtr krawedzi blokow (RFC 6386, rozdz. 15) ---- */

static inline int sclip1(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }
static inline int sclip2(int v) { return v < -16 ? -16 : v > 15 ? 15 : v; }
static inline int iabs(int v) { return v < 0 ? -v : v; }

static void f2(uint8_t *p, int s)
{
    int p1 = p[-2 * s], p0 = p[-s], q0 = p[0], q1 = p[s];
    int a = 3 * (q0 - p0) + sclip1(p1 - q1);
    int a1 = sclip2((a + 4) >> 3), a2 = sclip2((a + 3) >> 3);
    p[-s] = (uint8_t)clamp255(p0 + a2);
    p[0] = (uint8_t)clamp255(q0 - a1);
}
static void f4(uint8_t *p, int s)
{
    int p1 = p[-2 * s], p0 = p[-s], q0 = p[0], q1 = p[s];
    int a = 3 * (q0 - p0);
    int a1 = sclip2((a + 4) >> 3), a2 = sclip2((a + 3) >> 3), a3 = (a1 + 1) >> 1;
    p[-2 * s] = (uint8_t)clamp255(p1 + a3);
    p[-s] = (uint8_t)clamp255(p0 + a2);
    p[0] = (uint8_t)clamp255(q0 - a1);
    p[s] = (uint8_t)clamp255(q1 - a3);
}
static void f6(uint8_t *p, int s)
{
    int p2 = p[-3 * s], p1 = p[-2 * s], p0 = p[-s], q0 = p[0], q1 = p[s], q2 = p[2 * s];
    int a = sclip1(3 * (q0 - p0) + sclip1(p1 - q1));
    int a1 = (27 * a + 63) >> 7, a2 = (18 * a + 63) >> 7, a3 = (9 * a + 63) >> 7;
    p[-3 * s] = (uint8_t)clamp255(p2 + a3);
    p[-2 * s] = (uint8_t)clamp255(p1 + a2);
    p[-s] = (uint8_t)clamp255(p0 + a1);
    p[0] = (uint8_t)clamp255(q0 - a1);
    p[s] = (uint8_t)clamp255(q1 - a2);
    p[2 * s] = (uint8_t)clamp255(q2 - a3);
}
static int hev(const uint8_t *p, int s, int t)
{
    return iabs(p[-2 * s] - p[-s]) > t || iabs(p[s] - p[0]) > t;
}
static int needs(const uint8_t *p, int s, int t)
{
    return 4 * iabs(p[-s] - p[0]) + iabs(p[-2 * s] - p[s]) <= t;
}
static int needs2(const uint8_t *p, int s, int t, int it)
{
    int p3 = p[-4 * s], p2 = p[-3 * s], p1 = p[-2 * s], p0 = p[-s];
    int q0 = p[0], q1 = p[s], q2 = p[2 * s], q3 = p[3 * s];
    if (4 * iabs(p0 - q0) + iabs(p1 - q1) > t)
        return 0;
    return iabs(p3 - p2) <= it && iabs(p2 - p1) <= it && iabs(p1 - p0) <= it &&
           iabs(q3 - q2) <= it && iabs(q2 - q1) <= it && iabs(q1 - q0) <= it;
}
/* hs = krok w poprzek krawedzi, vs = krok wzdluz krawedzi; six = krawedz makrobloku */
static void loop_filter(uint8_t *p, int hs, int vs, int n, int th, int ith, int hv, int six)
{
    int t2 = 2 * th + 1;
    for (; n > 0; n--, p += vs)
        if (needs2(p, hs, t2, ith)) {
            if (hev(p, hs, hv))
                f2(p, hs);
            else if (six)
                f6(p, hs);
            else
                f4(p, hs);
        }
}
static void simple_filter(uint8_t *p, int hs, int vs, int th)
{
    int t2 = 2 * th + 1;
    for (int i = 0; i < 16; i++, p += vs)
        if (needs(p, hs, t2))
            f2(p, hs);
}

struct vp8 {
    int w, h, mbw, mbh, ys, uvs;
    uint8_t *y, *u, *v;
    int use_seg, upd_map, abs_delta, seg_q[4], seg_lf[4], seg_p[3];
    int simple, level, sharp, use_lfd, ref_lfd[4], mode_lfd[4];
    int q[4][6];                /* y1 dc/ac, y2 dc/ac, uv dc/ac dla segmentow */
    uint8_t prob[4][8][3][11];
    int use_skip, skip_p;
};

static void vp8_filter(struct vp8 *v, const uint8_t *info)
{
    if (v->level == 0)
        return;
    int lim[4][2], ilev[4][2], hv[4][2];
    for (int s = 0; s < 4; s++) {
        int base = v->level;
        if (v->use_seg)
            base = v->seg_lf[s] + (v->abs_delta ? 0 : v->level);
        for (int i4 = 0; i4 < 2; i4++) {
            int level = base;
            if (v->use_lfd) {
                level += v->ref_lfd[0];
                if (i4)
                    level += v->mode_lfd[0];
            }
            level = level < 0 ? 0 : level > 63 ? 63 : level;
            int il = level;
            if (v->sharp > 0) {
                il >>= v->sharp > 4 ? 2 : 1;
                if (il > 9 - v->sharp)
                    il = 9 - v->sharp;
            }
            if (il < 1)
                il = 1;
            ilev[s][i4] = il;
            lim[s][i4] = level ? 2 * level + il : 0;
            hv[s][i4] = level >= 40 ? 2 : level >= 15 ? 1 : 0;
        }
    }
    int ys = v->ys, uvs = v->uvs;
    for (int my = 0; my < v->mbh; my++) {
        for (int mx = 0; mx < v->mbw; mx++) {
            int f = info[my * v->mbw + mx], s = f & 3, i4 = f >> 2 & 1, inner = f >> 3 & 1;
            int L = lim[s][i4], il = ilev[s][i4], H = hv[s][i4];
            if (!L)
                continue;
            uint8_t *py = v->y + (size_t)my * 16 * ys + mx * 16;
            uint8_t *pu = v->u + (size_t)my * 8 * uvs + mx * 8;
            uint8_t *pv = v->v + (size_t)my * 8 * uvs + mx * 8;
            if (v->simple) {
                if (mx)
                    simple_filter(py, 1, ys, L + 4);
                if (inner)
                    for (int k = 4; k < 16; k += 4)
                        simple_filter(py + k, 1, ys, L);
                if (my)
                    simple_filter(py, ys, 1, L + 4);
                if (inner)
                    for (int k = 4; k < 16; k += 4)
                        simple_filter(py + k * ys, ys, 1, L);
                continue;
            }
            if (mx) {
                loop_filter(py, 1, ys, 16, L + 4, il, H, 1);
                loop_filter(pu, 1, uvs, 8, L + 4, il, H, 1);
                loop_filter(pv, 1, uvs, 8, L + 4, il, H, 1);
            }
            if (inner) {
                for (int k = 4; k < 16; k += 4)
                    loop_filter(py + k, 1, ys, 16, L, il, H, 0);
                loop_filter(pu + 4, 1, uvs, 8, L, il, H, 0);
                loop_filter(pv + 4, 1, uvs, 8, L, il, H, 0);
            }
            if (my) {
                loop_filter(py, ys, 1, 16, L + 4, il, H, 1);
                loop_filter(pu, uvs, 1, 8, L + 4, il, H, 1);
                loop_filter(pv, uvs, 1, 8, L + 4, il, H, 1);
            }
            if (inner) {
                for (int k = 4; k < 16; k += 4)
                    loop_filter(py + k * ys, ys, 1, 16, L, il, H, 0);
                loop_filter(pu + 4 * uvs, uvs, 1, 8, L, il, H, 0);
                loop_filter(pv + 4 * uvs, uvs, 1, 8, L, il, H, 0);
            }
        }
        BREATHE();
    }
}

/* Ramka bufora roboczego: wiersz nad blokiem i kolumna z lewej (z obrazu
 * albo 127/129 na brzegach, jak w specyfikacji) */
static void edges(const uint8_t *plane, int stride, uint8_t *d, int n, int mx, int my, int mbw)
{
    if (my == 0)
        memset(d - BPS - 1, 127, (size_t)n + 5);
    else {
        const uint8_t *t = plane + ((size_t)my * n - 1) * stride + mx * n;
        memcpy(d - BPS, t, (size_t)n);
        d[-BPS - 1] = mx ? t[-1] : 129;
        if (n == 16) {
            if (mx < mbw - 1)
                memcpy(d - BPS + 16, t + 16, 4);
            else
                memset(d - BPS + 16, t[15], 4);
        }
    }
    for (int j = 0; j < n; j++)
        d[j * BPS - 1] = mx ? plane[((size_t)my * n + j) * stride + mx * n - 1] : 129;
}

static int vp8_decode(const uint8_t *d, unsigned len, uint32_t **px, int *pw, int *ph)
{
    if (len < 10)
        return IMG_ECORRUPT;
    unsigned tag = le24(d);
    if (tag & 1)
        return IMG_EFORMAT;                 /* nie klatka kluczowa */
    unsigned p0size = tag >> 5;
    if (d[3] != 0x9d || d[4] != 0x01 || d[5] != 0x2a)
        return IMG_ECORRUPT;
    int w = (d[6] | d[7] << 8) & 0x3fff, h = (d[8] | d[9] << 8) & 0x3fff;
    if (!w || !h)
        return IMG_ECORRUPT;
    if ((uint64_t)w * h > MAX_PIXELS)
        return IMG_ETOOBIG;
    d += 10;
    len -= 10;
    if (p0size > len)
        return IMG_ECORRUPT;

    struct vp8 *v = kzalloc(sizeof *v);
    if (!v)
        return IMG_ENOMEM;
    struct bdec hb, part[8];
    bd_init(&hb, d, d + p0size);
    blit(&hb, 2);                           /* przestrzen kolorow, przycinanie */
    v->use_seg = bget(&hb, 128);
    if (v->use_seg) {
        v->upd_map = bget(&hb, 128);
        if (bget(&hb, 128)) {
            v->abs_delta = bget(&hb, 128);
            for (int s = 0; s < 4; s++)
                v->seg_q[s] = bopt(&hb, 7);
            for (int s = 0; s < 4; s++)
                v->seg_lf[s] = bopt(&hb, 6);
        }
        if (v->upd_map)
            for (int s = 0; s < 3; s++)
                v->seg_p[s] = bget(&hb, 128) ? blit(&hb, 8) : 255;
    }
    v->simple = bget(&hb, 128);
    v->level = blit(&hb, 6);
    v->sharp = blit(&hb, 3);
    v->use_lfd = bget(&hb, 128);
    if (v->use_lfd && bget(&hb, 128)) {
        for (int i = 0; i < 4; i++)
            if (bget(&hb, 128))
                v->ref_lfd[i] = bsigned(&hb, 6);
        for (int i = 0; i < 4; i++)
            if (bget(&hb, 128))
                v->mode_lfd[i] = bsigned(&hb, 6);
    }
    int np = 1 << blit(&hb, 2);
    {
        const uint8_t *ps = d + p0size, *end = d + len, *pd = ps + 3 * (np - 1);
        if (pd > end) {
            kfree(v);
            return IMG_ECORRUPT;
        }
        for (int i = 0; i < np; i++) {
            const uint8_t *pe = end;
            if (i < np - 1) {
                unsigned sz = le24(ps + 3 * i);
                pe = sz > (unsigned)(end - pd) ? end : pd + sz;
            }
            bd_init(&part[i], pd, pe);
            pd = pe;
        }
    }
    int base = blit(&hb, 7);
    int dy1dc = bopt(&hb, 4), dy2dc = bopt(&hb, 4), dy2ac = bopt(&hb, 4), duvdc = bopt(&hb, 4), duvac = bopt(&hb, 4);
    for (int s = 0; s < 4; s++) {
        int q = base;
        if (v->use_seg)
            q = v->seg_q[s] + (v->abs_delta ? 0 : base);
#define QC(x, m) ((x) < 0 ? 0 : (x) > (m) ? (m) : (x))
        v->q[s][0] = kDcQ[QC(q + dy1dc, 127)];
        v->q[s][1] = kAcQ[QC(q, 127)];
        v->q[s][2] = kDcQ[QC(q + dy2dc, 127)] * 2;
        v->q[s][3] = kAcQ[QC(q + dy2ac, 127)] * 101581 >> 16;
        if (v->q[s][3] < 8)
            v->q[s][3] = 8;
        v->q[s][4] = kDcQ[QC(q + duvdc, 117)];
        v->q[s][5] = kAcQ[QC(q + duvac, 127)];
    }
    bget(&hb, 128);                         /* odswiezenie prawdopodobienstw */
    for (int t = 0; t < 4; t++)
        for (int b = 0; b < 8; b++)
            for (int c = 0; c < 3; c++)
                for (int p = 0; p < 11; p++)
                    v->prob[t][b][c][p] = (uint8_t)(bget(&hb, kCoefUpd[t][b][c][p]) ? blit(&hb, 8) : kCoefP0[t][b][c][p]);
    v->use_skip = bget(&hb, 128);
    if (v->use_skip)
        v->skip_p = blit(&hb, 8);

    v->w = w;
    v->h = h;
    v->mbw = (w + 15) >> 4;
    v->mbh = (h + 15) >> 4;
    v->ys = v->mbw * 16;
    v->uvs = v->mbw * 8;
    size_t ysz = (size_t)v->ys * v->mbh * 16, uvsz = (size_t)v->uvs * v->mbh * 8;
    uint8_t *planes = kmalloc(ysz + 2 * uvsz);
    uint8_t *intra_t = kmalloc((size_t)v->mbw * 4);
    uint8_t *nz_t = kzalloc((size_t)v->mbw * 9);
    uint8_t *info = kmalloc((size_t)v->mbw * v->mbh);
    uint32_t *out = kmalloc((size_t)w * h * 4);
    int err = 0;
    if (!planes || !intra_t || !nz_t || !info || !out) {
        err = IMG_ENOMEM;
        goto done;
    }
    v->y = planes;
    v->u = planes + ysz;
    v->v = planes + ysz + uvsz;
    memset(intra_t, M_DC, (size_t)v->mbw * 4);

    uint8_t ybuf[21 * BPS], ubuf[9 * BPS], vbuf[9 * BPS];         /* blok + ramka z sasiadow */
    uint8_t *yd = ybuf + BPS + 8, *ud = ubuf + BPS + 8, *vd = vbuf + BPS + 8;
    int16_t coef[384];
    for (int my = 0; my < v->mbh; my++) {
        uint8_t left[4] = { M_DC, M_DC, M_DC, M_DC }, lnz[9] = { 0 };
        struct bdec *tb = &part[my & (np - 1)];
        for (int mx = 0; mx < v->mbw; mx++) {
            /* --- tryby (pierwsza partycja) --- */
            int seg = 0, skip = 0, ymode = 0;
            uint8_t modes[16];
            if (v->upd_map)
                seg = !bget(&hb, v->seg_p[0]) ? bget(&hb, v->seg_p[1]) : 2 + bget(&hb, v->seg_p[2]);
            if (v->use_skip)
                skip = bget(&hb, v->skip_p);
            int i4 = !bget(&hb, 145);
            uint8_t *top = intra_t + 4 * mx;
            if (!i4) {
                ymode = bget(&hb, 156) ? (bget(&hb, 128) ? M_TM : M_HE) : (bget(&hb, 163) ? M_VE : M_DC);
                memset(top, ymode, 4);
                memset(left, ymode, 4);
            } else
                for (int y = 0; y < 4; y++) {
                    int m = left[y];
                    for (int x = 0; x < 4; x++) {
                        const uint8_t *pr = kBModeP[top[x]][m];
                        int i = kYModes4[bget(&hb, pr[0])];
                        while (i > 0)
                            i = kYModes4[2 * i + bget(&hb, pr[i])];
                        m = -i;
                        top[x] = (uint8_t)m;
                        modes[y * 4 + x] = (uint8_t)m;
                    }
                    left[y] = (uint8_t)m;
                }
            int uvmode = !bget(&hb, 142) ? M_DC : !bget(&hb, 114) ? M_VE : bget(&hb, 183) ? M_TM : M_HE;

            /* --- wspolczynniki (partycja wiersza) --- */
            memset(coef, 0, sizeof coef);
            const int *q = v->q[seg];
            uint8_t *tn = nz_t + 9 * mx;
            int nonzero = 0;
            if (!skip) {
                int first = 0;
                const probs_t *pt = &v->prob[3];
                if (!i4) {
                    int16_t dc[16] = { 0 };
                    int nz = coeffs(tb, &v->prob[1], tn[8] + lnz[8], q[2], q[3], 0, dc);
                    tn[8] = lnz[8] = nz > 0;
                    if (nz > 1)
                        wht(dc, coef);
                    else {
                        int d0 = (dc[0] + 3) >> 3;
                        for (int i = 0; i < 16; i++)
                            coef[i * 16] = (int16_t)d0;
                    }
                    first = 1;
                    pt = &v->prob[0];
                }
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) {
                        int nz = coeffs(tb, pt, tn[x] + lnz[y], q[0], q[1], first, coef + (y * 4 + x) * 16);
                        tn[x] = lnz[y] = nz > first;
                    }
                for (int ch = 0; ch < 2; ch++)
                    for (int y = 0; y < 2; y++)
                        for (int x = 0; x < 2; x++) {
                            int nz = coeffs(tb, &v->prob[2], tn[4 + ch * 2 + x] + lnz[4 + ch * 2 + y], q[4], q[5], 0,
                                            coef + 256 + ch * 64 + (y * 2 + x) * 16);
                            tn[4 + ch * 2 + x] = lnz[4 + ch * 2 + y] = nz > 0;
                        }
                for (int i = 0; i < 384 && !nonzero; i++)
                    nonzero = coef[i] != 0;
            } else {
                memset(tn, 0, 8);
                memset(lnz, 0, 8);
                if (!i4)
                    tn[8] = lnz[8] = 0;
            }
            info[my * v->mbw + mx] = (uint8_t)(seg | i4 << 2 | (i4 || nonzero) << 3);

            /* --- odtworzenie pikseli --- */
            edges(v->y, v->ys, yd, 16, mx, my, v->mbw);
            edges(v->u, v->uvs, ud, 8, mx, my, v->mbw);
            edges(v->v, v->uvs, vd, 8, mx, my, v->mbw);
            if (!i4) {
                pred_big(yd, 16, ymode, my > 0, mx > 0);
                for (int b = 0; b < 16; b++)
                    idct_add(coef + b * 16, yd + (b >> 2) * 4 * BPS + (b & 3) * 4);
            } else {
                for (int r = 3; r < 16; r += 4)     /* piksele nad-z-prawej dla kolumny 3 */
                    memcpy(yd + r * BPS + 16, yd - BPS + 16, 4);
                for (int b = 0; b < 16; b++) {
                    uint8_t *dst = yd + (b >> 2) * 4 * BPS + (b & 3) * 4;
                    pred4(dst, modes[b]);
                    idct_add(coef + b * 16, dst);
                }
            }
            pred_big(ud, 8, uvmode, my > 0, mx > 0);
            pred_big(vd, 8, uvmode, my > 0, mx > 0);
            for (int b = 0; b < 4; b++) {
                idct_add(coef + 256 + b * 16, ud + (b >> 1) * 4 * BPS + (b & 1) * 4);
                idct_add(coef + 320 + b * 16, vd + (b >> 1) * 4 * BPS + (b & 1) * 4);
            }
            for (int j = 0; j < 16; j++)
                memcpy(v->y + ((size_t)my * 16 + j) * v->ys + mx * 16, yd + j * BPS, 16);
            for (int j = 0; j < 8; j++) {
                memcpy(v->u + ((size_t)my * 8 + j) * v->uvs + mx * 8, ud + j * BPS, 8);
                memcpy(v->v + ((size_t)my * 8 + j) * v->uvs + mx * 8, vd + j * BPS, 8);
            }
        }
        BREATHE();
    }
    vp8_filter(v, info);

    /* YUV -> RGB, kolor (U, V) ma polowe rozdzielczosci: probki laczymy
     * z sasiednimi w proporcji 9:3:3:1 (jak libwebp "fancy upsampling") */
    int cw = (w + 1) >> 1, chh = (h + 1) >> 1;
    for (int y = 0; y < h; y++) {
        int j0 = y >> 1, j1 = (y & 1) ? j0 + 1 : j0 - 1;
        if (j1 < 0) j1 = 0;
        if (j1 >= chh) j1 = chh - 1;
        const uint8_t *yr = v->y + (size_t)y * v->ys;
        const uint8_t *u0 = v->u + (size_t)j0 * v->uvs, *u1 = v->u + (size_t)j1 * v->uvs;
        const uint8_t *v0 = v->v + (size_t)j0 * v->uvs, *v1 = v->v + (size_t)j1 * v->uvs;
        uint32_t *o = out + (size_t)y * w;
        for (int x = 0; x < w; x++) {
            int i0 = x >> 1, i1 = (x & 1) ? i0 + 1 : i0 - 1;
            if (i1 < 0) i1 = 0;
            if (i1 >= cw) i1 = cw - 1;
            int U = (9 * u0[i0] + 3 * u0[i1] + 3 * u1[i0] + u1[i1] + 8) >> 4;
            int V = (9 * v0[i0] + 3 * v0[i1] + 3 * v1[i0] + v1[i1] + 8) >> 4;
            int Y = (yr[x] * 19077) >> 8;
            int r = (Y + ((V * 26149) >> 8) - 14234) >> 6;
            int g = (Y - ((U * 6419) >> 8) - ((V * 13320) >> 8) + 8708) >> 6;
            int b = (Y + ((U * 33050) >> 8) - 17685) >> 6;
            o[x] = 0xff000000u | (uint32_t)clamp255(r) << 16 | (uint32_t)clamp255(g) << 8 | (uint32_t)clamp255(b);
        }
        if ((y & 63) == 63)
            BREATHE();
    }
done:
    kfree(planes);
    kfree(intra_t);
    kfree(nz_t);
    kfree(info);
    kfree(v);
    if (err) {
        kfree(out);
        return err;
    }
    *px = out;
    *pw = w;
    *ph = h;
    return 0;
}

/* ============================ kontener RIFF ============================ */

struct frame { const uint8_t *vp8, *vp8l, *alph; unsigned vlen, llen, alen; };

static void scan_chunks(const uint8_t *p, unsigned len, struct frame *f)
{
    unsigned pos = 0;
    while (pos + 8 <= len && !f->vp8 && !f->vp8l) {
        const uint8_t *c = p + pos;
        unsigned sz = le32(c + 4);
        if (sz > len - pos - 8)
            sz = len - pos - 8;
        if (!memcmp(c, "ALPH", 4)) {
            f->alph = c + 8;
            f->alen = sz;
        } else if (!memcmp(c, "VP8 ", 4)) {
            f->vp8 = c + 8;
            f->vlen = sz;
        } else if (!memcmp(c, "VP8L", 4)) {
            f->vp8l = c + 8;
            f->llen = sz;
        }
        pos += 8 + sz + (sz & 1);
    }
}

int webp_decode(const uint8_t *d, unsigned len, struct image *img)
{
    if (len < 20 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WEBP", 4))
        return IMG_EFORMAT;
    unsigned riff = le32(d + 4);
    if (riff + 8 < len && riff >= 4)
        len = riff + 8;
    struct frame f = { 0 };
    int cw = 0, ch = 0, fx = 0, fy = 0, anim = 0;
    unsigned pos = 12;
    while (pos + 8 <= len) {
        const uint8_t *c = d + pos, *p = c + 8;
        unsigned sz = le32(c + 4);
        if (sz > len - pos - 8)
            sz = len - pos - 8;
        if (!memcmp(c, "VP8X", 4) && sz >= 10) {
            cw = 1 + (int)le24(p + 4);
            ch = 1 + (int)le24(p + 7);
        } else if (!memcmp(c, "ANMF", 4) && sz >= 16) {
            fx = 2 * (int)le24(p);
            fy = 2 * (int)le24(p + 3);
            anim = 1;
            scan_chunks(p + 16, sz - 16, &f);       /* pierwsza klatka */
            break;
        } else if (!memcmp(c, "ALPH", 4) || !memcmp(c, "VP8 ", 4) || !memcmp(c, "VP8L", 4)) {
            scan_chunks(c, len - pos, &f);
            break;
        }
        pos += 8 + sz + (sz & 1);
    }

    uint32_t *px = 0;
    int w = 0, h = 0, err;
    if (f.vp8l)
        err = vp8l_decode(f.vp8l, f.llen, &px, &w, &h);
    else if (f.vp8) {
        err = vp8_decode(f.vp8, f.vlen, &px, &w, &h);
        if (!err && f.alph) {
            uint8_t *a = kmalloc((size_t)w * h);
            if (a && alph_decode(f.alph, f.alen, a, w, h) == 0)
                for (size_t i = 0; i < (size_t)w * h; i++)
                    px[i] = (px[i] & 0xffffff) | (uint32_t)a[i] << 24;
            kfree(a);                               /* zla alfa = obrazek bez przezroczystosci */
        }
    } else
        return IMG_ECORRUPT;
    if (err)
        return err;

    /* Klatka animacji mniejsza niz plotno: reszta przezroczysta */
    if (anim && cw > 0 && ch > 0 && (cw != w || ch != h || fx || fy)) {
        if ((uint64_t)cw * ch > MAX_PIXELS) {
            kfree(px);
            return IMG_ETOOBIG;
        }
        uint32_t *c = kzalloc((size_t)cw * ch * 4);
        if (!c) {
            kfree(px);
            return IMG_ENOMEM;
        }
        for (int y = 0; y < h && fy + y < ch; y++)
            for (int x = 0; x < w && fx + x < cw; x++)
                c[(size_t)(fy + y) * cw + fx + x] = px[(size_t)y * w + x];
        kfree(px);
        px = c;
        w = cw;
        h = ch;
    }
    int alpha = 0;
    for (size_t i = 0; i < (size_t)w * h && !alpha; i++)
        alpha = (px[i] >> 24) != 255;
    if (!alpha)
        for (size_t i = 0; i < (size_t)w * h; i++)
            px[i] &= 0xffffff;
    img->w = w;
    img->h = h;
    img->px = px;
    img->alpha = alpha;
    return 0;
}

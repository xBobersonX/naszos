/* Dekodery obrazkow - patrz image.h.
 *
 *  GIF  - kolory z palety (do 256), kompresja LZW: slownik rosnie w trakcie
 *         czytania i kazdy kod oznacza coraz dluzszy ciag pikseli.
 *  PNG  - kompresja DEFLATE (jak ZIP): kody Huffmana (czeste bajty maja
 *         krotkie kody) + powtorzenia "skopiuj N bajtow sprzed M bajtow".
 *         Przed kompresja kazdy wiersz jest "filtrowany" (zapisany jako
 *         roznica wzgledem sasiadow) - trzeba to odwrocic.
 *  JPEG - zdjecia. Obraz pociety na kwadraty 8x8, kazdy zapisany jako suma
 *         "fal" (transformata kosinusowa, DCT); drobne fale sa zaokraglone
 *         (stad male pliki). Kolor w ukladzie jasnosc + dwa odcienie (YCbCr).
 *         JPEG "progresywny" przychodzi w kilku przebiegach (najpierw
 *         zgrubnie, potem dokladniej) - zbieramy wspolczynniki wszystkich
 *         blokow i dopiero na koncu zamieniamy je na piksele.
 * Wszystko na liczbach calkowitych. */

#include "image.h"
#include "heap.h"
#include "string.h"
#include "thread.h"

/* Zadania jadra NaszOS nie sa wywlaszczane - dlugie dekodowanie co chwile
 * oddaje procesor, zeby pulpit i myszka dzialaly plynnie */
#define BREATHE() thread_yield()

#define MAX_PIXELS (4u * 1024 * 1024)

const char *image_strerror(int err)
{
    switch (err) {
    case IMG_EFORMAT:      return "nieznany format obrazka";
    case IMG_ECORRUPT:     return "uszkodzony obrazek";
    case IMG_ENOMEM:       return "za malo pamieci";
    case IMG_EPROGRESSIVE: return "JPEG progresywny - jeszcze nie umiemy";
    case IMG_ETOOBIG:      return "obrazek za duzy";
    default:               return "blad obrazka";
    }
}

void image_free(struct image *img)
{
    kfree(img->px);
    img->px = 0;
    img->w = img->h = 0;
}

static inline uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

static uint32_t rgb(int r, int g, int b) { return (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b; }

/* Kolor z przezroczystoscia (0-255) na bialym tle */
static uint32_t over_white(int r, int g, int b, int a)
{
    return rgb((r * a + 255 * (255 - a)) / 255, (g * a + 255 * (255 - a)) / 255, (b * a + 255 * (255 - a)) / 255);
}

static int alloc_image(struct image *img, int w, int h)
{
    if (w <= 0 || h <= 0)
        return IMG_ECORRUPT;
    if ((uint64_t)w * (uint64_t)h > MAX_PIXELS)
        return IMG_ETOOBIG;
    img->w = w;
    img->h = h;
    img->px = kmalloc((size_t)w * (size_t)h * 4);
    return img->px ? 0 : IMG_ENOMEM;
}

/* ============================ GIF ============================ */

static int gif_decode(const uint8_t *d, unsigned len, struct image *img)
{
    if (len < 13)
        return IMG_ECORRUPT;
    int w = le16(d + 6), h = le16(d + 8);
    uint8_t palette[256 * 3];
    memset(palette, 0, sizeof(palette));
    unsigned pos = 13;
    if (d[10] & 0x80) {                          /* globalna paleta */
        unsigned n = 2u << (d[10] & 7);
        if (pos + 3 * n > len)
            return IMG_ECORRUPT;
        memcpy(palette, d + pos, 3 * n);
        pos += 3 * n;
    }
    int err = alloc_image(img, w, h);
    if (err)
        return err;
    for (int i = 0; i < w * h; i++)
        img->px[i] = 0xFFFFFF;
    int transparent = -1;

    while (pos < len) {
        uint8_t b = d[pos++];
        if (b == 0x3B)                           /* koniec pliku */
            break;
        if (b == 0x21) {                         /* rozszerzenie */
            if (pos + 1 > len)
                break;
            uint8_t label = d[pos++];
            if (label == 0xF9 && pos + 5 < len && d[pos] == 4 && (d[pos + 1] & 1))
                transparent = d[pos + 4];        /* ktory kolor jest "przezroczysty" */
            while (pos < len && d[pos])
                pos += d[pos] + 1u;
            pos++;
            continue;
        }
        if (b != 0x2C || pos + 9 > len)
            break;
        /* Obrazek (bierzemy tylko pierwsza klatke animacji) */
        int ix = le16(d + pos), iy = le16(d + pos + 2), iw = le16(d + pos + 4), ih = le16(d + pos + 6);
        uint8_t flags = d[pos + 8];
        pos += 9;
        uint8_t local[256 * 3];
        const uint8_t *pal = palette;
        if (flags & 0x80) {
            unsigned n = 2u << (flags & 7);
            if (pos + 3 * n > len)
                return IMG_ECORRUPT;
            memcpy(local, d + pos, 3 * n);
            pal = local;
            pos += 3 * n;
        }
        if (pos >= len || iw <= 0 || ih <= 0)
            return IMG_ECORRUPT;
        int min = d[pos++];
        if (min < 2 || min > 8)
            return IMG_ECORRUPT;
        /* Dane LZW sa pociete na kawalki po najwyzej 255 bajtow */
        unsigned total = 0;
        for (unsigned p = pos; p < len && d[p]; p += d[p] + 1u)
            total += d[p];
        uint8_t *data = kmalloc(total + 4);
        uint8_t *idx = kmalloc((size_t)iw * (size_t)ih);
        static uint16_t prefix[4096];
        static uint8_t suffix[4096], stack[4097];
        if (!data || !idx) {
            kfree(data);
            kfree(idx);
            return IMG_ENOMEM;
        }
        unsigned n = 0;
        while (pos < len && d[pos]) {
            unsigned sz = d[pos];
            if (pos + 1 + sz > len)
                break;
            memcpy(data + n, d + pos + 1, sz);
            n += sz;
            pos += sz + 1;
        }
        memset(idx, 0, (size_t)iw * (size_t)ih);

        int clear = 1 << min, eoi = clear + 1, next = eoi + 1, size = min + 1, old = -1, first = 0;
        for (int i = 0; i < clear; i++) {
            prefix[i] = 0;
            suffix[i] = (uint8_t)i;
        }
        unsigned out = 0, total_px = (unsigned)iw * (unsigned)ih, bitpos = 0;
        while (out < total_px && bitpos + (unsigned)size <= n * 8) {
            int code = 0;
            for (int k = 0; k < size; k++, bitpos++)
                code |= ((data[bitpos >> 3] >> (bitpos & 7)) & 1) << k;
            if (code == clear) {
                size = min + 1;
                next = eoi + 1;
                old = -1;
                continue;
            }
            if (code == eoi)
                break;
            if (old < 0) {
                if (code >= clear)
                    break;
                idx[out++] = (uint8_t)code;
                old = first = code;
                continue;
            }
            if (code > next)
                break;                           /* uszkodzone dane */
            int in = code, sp = 0;
            if (code == next) {                  /* kod, ktorego jeszcze nie ma: "stary + jego 1. znak" */
                stack[sp++] = (uint8_t)first;
                code = old;
            }
            while (code >= clear && sp < 4096) {
                stack[sp++] = suffix[code];
                code = prefix[code];
            }
            first = code;
            stack[sp++] = (uint8_t)first;
            while (sp && out < total_px)
                idx[out++] = stack[--sp];
            if (next < 4096) {
                prefix[next] = (uint16_t)old;
                suffix[next] = (uint8_t)first;
                next++;
                if (next == (1 << size) && size < 12)
                    size++;
            }
            old = in;
        }
        /* Przeplot: wiersze zapisane co 8, potem co 8 od 4, co 4 od 2, co 2 od 1 */
        int interlace = flags & 0x40, row = 0, pass = 0;
        static const int start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };
        for (int r = 0; r < ih; r++) {
            int y = r;
            if (interlace) {
                y = row;
                row += step[pass];
                while (row >= ih && pass < 3) {
                    pass++;
                    row = start[pass];
                }
            }
            for (int x = 0; x < iw; x++) {
                int c = idx[r * iw + x];
                int px = ix + x, py = iy + y;
                if (c == transparent || px >= w || py >= h)
                    continue;
                img->px[py * w + px] = rgb(pal[c * 3], pal[c * 3 + 1], pal[c * 3 + 2]);
            }
        }
        kfree(data);
        kfree(idx);
        return 0;
    }
    return 0;
}

/* ============================ DEFLATE ============================ */

struct inflate {
    const uint8_t *in;
    unsigned in_len, pos;
    uint32_t bitbuf;
    int bitcnt, err;
    uint8_t *out;
    unsigned out_len, out_pos;
};

struct huffman {
    uint16_t counts[16];                 /* ile kodow danej dlugosci */
    uint16_t symbols[288];
};

static unsigned bits(struct inflate *s, int n)
{
    while (s->bitcnt < n) {
        if (s->pos >= s->in_len) {
            s->err = 1;
            return 0;
        }
        s->bitbuf |= (uint32_t)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    unsigned v = s->bitbuf & ((1u << n) - 1);
    s->bitbuf >>= n;
    s->bitcnt -= n;
    return v;
}

static void huff_build(struct huffman *h, const uint8_t *lengths, int n)
{
    uint16_t offs[16];
    memset(h->counts, 0, sizeof(h->counts));
    for (int i = 0; i < n; i++)
        h->counts[lengths[i]]++;
    h->counts[0] = 0;
    offs[1] = 0;
    for (int l = 1; l < 15; l++)
        offs[l + 1] = (uint16_t)(offs[l] + h->counts[l]);
    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbols[offs[lengths[i]]++] = (uint16_t)i;
}

/* Kody Huffmana czytamy bit po bicie: kody tej samej dlugosci sa kolejnymi liczbami */
static int huff_decode(struct inflate *s, const struct huffman *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= (int)bits(s, 1);
        int count = h->counts[len];
        if (code - count < first)
            return h->symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s->err)
            return -1;
    }
    return -1;
}

static int inflate_codes(struct inflate *s, const struct huffman *lit, const struct huffman *dist)
{
    static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
    static const uint8_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                      3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
    static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                        8193, 12289, 16385, 24577 };
    static const uint8_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                      7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
    for (;;) {
        int sym = huff_decode(s, lit);
        if (sym < 0 || s->err)
            return -1;
        if ((s->out_pos & 0xFFFF) == 0xFFFF)
            BREATHE();
        if (sym < 256) {                                    /* zwykly bajt */
            if (s->out_pos >= s->out_len)
                return -1;
            s->out[s->out_pos++] = (uint8_t)sym;
        } else if (sym == 256) {                            /* koniec bloku */
            return 0;
        } else {                                            /* "skopiuj len bajtow sprzed dist" */
            sym -= 257;
            if (sym >= 29)
                return -1;
            unsigned len = lbase[sym] + bits(s, lext[sym]);
            int ds = huff_decode(s, dist);
            if (ds < 0 || ds >= 30)
                return -1;
            unsigned d = dbase[ds] + bits(s, dext[ds]);
            if (d > s->out_pos || s->out_pos + len > s->out_len || s->err)
                return -1;
            for (unsigned i = 0; i < len; i++, s->out_pos++)
                s->out[s->out_pos] = s->out[s->out_pos - d];
        }
    }
}

static int inflate(const uint8_t *in, unsigned in_len, uint8_t *out, unsigned out_len)
{
    struct inflate s = { in, in_len, 0, 0, 0, 0, out, out_len, 0 };
    static struct huffman lit, dist;
    uint8_t lengths[320];
    int last;
    do {
        last = (int)bits(&s, 1);
        int type = (int)bits(&s, 2);
        if (s.err)
            return -1;
        if (type == 0) {                                    /* bez kompresji */
            s.bitbuf = 0;
            s.bitcnt = 0;
            if (s.pos + 4 > s.in_len)
                return -1;
            unsigned len = s.in[s.pos] | s.in[s.pos + 1] << 8;
            s.pos += 4;
            if (s.pos + len > s.in_len || s.out_pos + len > s.out_len)
                return -1;
            memcpy(s.out + s.out_pos, s.in + s.pos, len);
            s.pos += len;
            s.out_pos += len;
        } else if (type == 1) {                             /* stale kody Huffmana */
            int i = 0;
            for (; i < 144; i++) lengths[i] = 8;
            for (; i < 256; i++) lengths[i] = 9;
            for (; i < 280; i++) lengths[i] = 7;
            for (; i < 288; i++) lengths[i] = 8;
            huff_build(&lit, lengths, 288);
            for (i = 0; i < 30; i++) lengths[i] = 5;
            huff_build(&dist, lengths, 30);
            if (inflate_codes(&s, &lit, &dist))
                return -1;
        } else if (type == 2) {                             /* kody zapisane w pliku */
            static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            int nlen = (int)bits(&s, 5) + 257, ndist = (int)bits(&s, 5) + 1, ncode = (int)bits(&s, 4) + 4;
            if (nlen > 286 || ndist > 30)
                return -1;
            memset(lengths, 0, 19);
            for (int i = 0; i < ncode; i++)
                lengths[order[i]] = (uint8_t)bits(&s, 3);
            struct huffman lencode;
            huff_build(&lencode, lengths, 19);
            int i = 0;
            while (i < nlen + ndist) {
                int sym = huff_decode(&s, &lencode);
                if (sym < 0 || s.err)
                    return -1;
                if (sym < 16) {
                    lengths[i++] = (uint8_t)sym;
                    continue;
                }
                uint8_t val = 0;
                int rep;
                if (sym == 16) {
                    if (i == 0)
                        return -1;
                    val = lengths[i - 1];
                    rep = 3 + (int)bits(&s, 2);
                } else if (sym == 17) {
                    rep = 3 + (int)bits(&s, 3);
                } else {
                    rep = 11 + (int)bits(&s, 7);
                }
                if (i + rep > nlen + ndist)
                    return -1;
                while (rep--)
                    lengths[i++] = val;
            }
            huff_build(&lit, lengths, nlen);
            huff_build(&dist, lengths + nlen, ndist);
            if (inflate_codes(&s, &lit, &dist))
                return -1;
        } else {
            return -1;
        }
    } while (!last);
    return (int)s.out_pos;
}

/* ============================ PNG ============================ */

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

struct png_info {
    int w, h, depth, ctype, channels;
    uint8_t palette[256 * 3];
    uint8_t alpha[256];
};

/* Odwraca filtry wierszy i wpisuje piksele (pw x ph) do obrazka co (xs, ys) od (x0, y0) */
static int png_pass(const struct png_info *p, uint8_t *raw, int pw, int ph,
                    int x0, int y0, int xs, int ys, struct image *img)
{
    unsigned bpp_bits = (unsigned)(p->channels * p->depth);
    unsigned rowbytes = (pw * bpp_bits + 7) / 8, bpp = bpp_bits >= 8 ? bpp_bits / 8 : 1;
    uint8_t *prev = 0;
    for (int y = 0; y < ph; y++) {
        uint8_t *row = raw + (size_t)y * (rowbytes + 1);
        int filter = row[0];
        uint8_t *cur = row + 1;
        for (unsigned i = 0; i < rowbytes; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0, b = prev ? prev[i] : 0, c = (prev && i >= bpp) ? prev[i - bpp] : 0;
            switch (filter) {
            case 0: break;
            case 1: cur[i] = (uint8_t)(cur[i] + a); break;
            case 2: cur[i] = (uint8_t)(cur[i] + b); break;
            case 3: cur[i] = (uint8_t)(cur[i] + (a + b) / 2); break;
            case 4: cur[i] = (uint8_t)(cur[i] + paeth(a, b, c)); break;
            default: return IMG_ECORRUPT;
            }
        }
        prev = cur;
        if ((y & 15) == 15)
            BREATHE();
        for (int x = 0; x < pw; x++) {
            int r, g, bl, al = 255;
            if (p->depth < 8) {                       /* kilka pikseli w bajcie */
                unsigned bit = (unsigned)x * (unsigned)p->depth;
                int v = (cur[bit / 8] >> (8 - p->depth - bit % 8)) & ((1 << p->depth) - 1);
                if (p->ctype == 3) {
                    r = p->palette[v * 3]; g = p->palette[v * 3 + 1]; bl = p->palette[v * 3 + 2];
                    al = p->alpha[v];
                } else {
                    r = g = bl = v * 255 / ((1 << p->depth) - 1);
                }
            } else {
                int step = p->depth / 8;                  /* 16 bitow: bierzemy starszy bajt */
                const uint8_t *s = cur + (size_t)x * (unsigned)p->channels * (unsigned)step;
                switch (p->ctype) {
                case 0: r = g = bl = s[0]; break;
                case 2: r = s[0]; g = s[step]; bl = s[2 * step]; break;
                case 3: r = p->palette[s[0] * 3]; g = p->palette[s[0] * 3 + 1]; bl = p->palette[s[0] * 3 + 2];
                        al = p->alpha[s[0]]; break;
                case 4: r = g = bl = s[0]; al = s[step]; break;
                default: r = s[0]; g = s[step]; bl = s[2 * step]; al = s[3 * step]; break;
                }
            }
            int px = x0 + x * xs, py = y0 + y * ys;
            img->px[py * img->w + px] = al == 255 ? rgb(r, g, bl) : over_white(r, g, bl, al);
        }
    }
    return 0;
}

static int png_decode(const uint8_t *d, unsigned len, struct image *img)
{
    struct png_info p;
    memset(&p, 0, sizeof(p));
    memset(p.alpha, 255, sizeof(p.alpha));
    int interlace = 0;
    unsigned idat_total = 0;
    for (unsigned pos = 8; pos + 12 <= len;) {             /* 1. przebieg: naglowek, paleta, rozmiar danych */
        uint32_t clen = be32(d + pos);
        const uint8_t *type = d + pos + 4, *c = d + pos + 8;
        if (clen > len - pos - 12)
            return IMG_ECORRUPT;
        if (memcmp(type, "IHDR", 4) == 0 && clen >= 13) {
            p.w = (int)be32(c);
            p.h = (int)be32(c + 4);
            p.depth = c[8];
            p.ctype = c[9];
            interlace = c[12];
        } else if (memcmp(type, "PLTE", 4) == 0) {
            memcpy(p.palette, c, clen < sizeof(p.palette) ? clen : sizeof(p.palette));
        } else if (memcmp(type, "tRNS", 4) == 0 && clen <= 256) {
            memcpy(p.alpha, c, clen);
        } else if (memcmp(type, "IDAT", 4) == 0) {
            idat_total += clen;
        }
        pos += 12 + clen;
    }
    static const int channels[7] = { 1, 0, 3, 1, 2, 0, 4 };
    if (p.ctype > 6 || !channels[p.ctype] || !(p.depth == 1 || p.depth == 2 || p.depth == 4 ||
                                                 p.depth == 8 || p.depth == 16))
        return IMG_ECORRUPT;
    p.channels = channels[p.ctype];
    if ((p.depth < 8 && p.ctype != 0 && p.ctype != 3) || !idat_total)
        return IMG_ECORRUPT;
    int err = alloc_image(img, p.w, p.h);
    if (err)
        return err;

    /* Rozmiar danych po rozpakowaniu (z przeplotem Adam7 - 7 malych obrazkow) */
    static const int ax[7] = { 0, 4, 0, 2, 0, 1, 0 }, ay[7] = { 0, 0, 4, 0, 2, 0, 1 };
    static const int sx[7] = { 8, 8, 4, 4, 2, 2, 1 }, sy[7] = { 8, 8, 8, 4, 4, 2, 2 };
    int passes = interlace ? 7 : 1;
    size_t raw_len = 0;
    for (int i = 0; i < passes; i++) {
        int x0 = interlace ? ax[i] : 0, y0 = interlace ? ay[i] : 0, xs = interlace ? sx[i] : 1, ys = interlace ? sy[i] : 1;
        int pw = (p.w - x0 + xs - 1) / xs, ph = (p.h - y0 + ys - 1) / ys;
        if (pw > 0 && ph > 0)
            raw_len += (size_t)ph * ((pw * (unsigned)(p.channels * p.depth) + 7) / 8 + 1);
    }
    uint8_t *z = kmalloc(idat_total);
    uint8_t *raw = kmalloc(raw_len + 1);
    if (!z || !raw) {
        kfree(z);
        kfree(raw);
        image_free(img);
        return IMG_ENOMEM;
    }
    unsigned zn = 0;
    for (unsigned pos = 8; pos + 12 <= len;) {             /* 2. przebieg: sklejamy dane IDAT */
        uint32_t clen = be32(d + pos);
        if (memcmp(d + pos + 4, "IDAT", 4) == 0) {
            memcpy(z + zn, d + pos + 8, clen);
            zn += clen;
        }
        pos += 12 + clen;
    }
    /* Strumien zlib: 2 bajty naglowka, potem DEFLATE */
    int got = zn > 2 && (z[0] & 15) == 8 ? inflate(z + 2, zn - 2, raw, (unsigned)raw_len) : -1;
    kfree(z);
    if (got != (int)raw_len) {
        kfree(raw);
        image_free(img);
        return IMG_ECORRUPT;
    }
    uint8_t *r = raw;
    for (int i = 0; i < passes && !err; i++) {
        int x0 = interlace ? ax[i] : 0, y0 = interlace ? ay[i] : 0, xs = interlace ? sx[i] : 1, ys = interlace ? sy[i] : 1;
        int pw = (p.w - x0 + xs - 1) / xs, ph = (p.h - y0 + ys - 1) / ys;
        if (pw <= 0 || ph <= 0)
            continue;
        err = png_pass(&p, r, pw, ph, x0, y0, xs, ys, img);
        r += (size_t)ph * ((pw * (unsigned)(p.channels * p.depth) + 7) / 8 + 1);
    }
    kfree(raw);
    if (err)
        image_free(img);
    return err;
}

/* ============================ JPEG ============================ */

static const uint8_t zigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* cos((2x+1)u*pi/16) * C(u) / 2 * 4096 - policzone raz (Python) i wpisane */
static const int32_t idct_table[8][8] = {
    { 1448, 2009, 1892, 1703, 1448, 1138, 784, 400 },
    { 1448, 1703, 784, -400, -1448, -2009, -1892, -1138 },
    { 1448, 1138, -784, -2009, -1448, 400, 1892, 1703 },
    { 1448, 400, -1892, -1138, 1448, 1703, -784, -2009 },
    { 1448, -400, -1892, 1138, 1448, -1703, -784, 2009 },
    { 1448, -1138, -784, 2009, -1448, -400, 1892, -1703 },
    { 1448, -1703, 784, 400, -1448, 2009, -1892, 1138 },
    { 1448, -2009, 1892, -1703, 1448, -1138, 784, -400 },
};

struct jhuff {
    uint8_t vals[256];
    int mincode[17], maxcode[18], valptr[17];
    int ready;
};

struct jcomp {
    int id, h, v, tq, td, ta;
    int pw, ph;                  /* rozmiar planu (wielokrotnosc 8) */
    uint8_t *plane;
    int dcpred;
    int16_t *coefs;              /* progresywny: wspolczynniki wszystkich blokow */
    int bw, bh;                  /* bloki w planie */
};

struct jpeg {
    const uint8_t *d;
    unsigned len, pos;
    uint16_t qt[4][64];
    struct jhuff dc[4], ac[4];
    struct jcomp c[3];
    int nc, w, h, hmax, vmax, restart;
    uint32_t bitbuf;
    int bitcnt, marker;
    int progressive, eobrun, ss, se, ah, al;   /* biezacy przebieg JPEG progresywnego */
};

static int jbit(struct jpeg *j)
{
    if (j->bitcnt == 0) {
        uint8_t b = 0;
        if (!j->marker && j->pos < j->len) {
            b = j->d[j->pos];
            if (b == 0xFF) {
                uint8_t nb = j->pos + 1 < j->len ? j->d[j->pos + 1] : 0;
                if (nb == 0) {
                    j->pos += 2;                             /* FF 00 = zwykly bajt FF */
                } else {
                    j->marker = 1;                           /* znacznik (np. RST) - dalej zera */
                    b = 0;
                }
            } else {
                j->pos++;
            }
        }
        j->bitbuf = b;
        j->bitcnt = 8;
    }
    j->bitcnt--;
    return (int)((j->bitbuf >> j->bitcnt) & 1);
}

static int jbits(struct jpeg *j, int n)
{
    int v = 0;
    while (n--)
        v = v << 1 | jbit(j);
    return v;
}

static int jdecode(struct jpeg *j, const struct jhuff *h)
{
    int code = jbit(j);
    for (int l = 1; l <= 16; l++) {
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l])
            return h->vals[h->valptr[l] + code - h->mincode[l]];
        code = code << 1 | jbit(j);
    }
    return -1;
}

static int extend(int v, int s)
{
    return s == 0 ? 0 : v < (1 << (s - 1)) ? v - (1 << s) + 1 : v;
}

static void jidct(struct jcomp *c, const int32_t coef[64], int bx, int by);

/* Jeden blok 8x8: wspolczynniki -> odwrotna DCT -> piksele w planie */
static int jblock(struct jpeg *j, struct jcomp *c, int bx, int by)
{
    int32_t coef[64];
    memset(coef, 0, sizeof(coef));
    const uint16_t *q = j->qt[c->tq];
    int t = jdecode(j, &j->dc[c->td]);
    if (t < 0 || t > 16)
        return -1;
    c->dcpred += extend(jbits(j, t), t);
    coef[0] = c->dcpred * q[0];
    for (int k = 1; k < 64;) {
        int rs = jdecode(j, &j->ac[c->ta]);
        if (rs < 0)
            return -1;
        int r = rs >> 4, s = rs & 15;
        if (s == 0) {
            if (r != 15)
                break;                                       /* reszta bloku to zera */
            k += 16;
            continue;
        }
        k += r;
        if (k > 63)
            return -1;
        coef[zigzag[k]] = extend(jbits(j, s), s) * q[zigzag[k]];
        k++;
    }
    jidct(c, coef, bx, by);
    return 0;
}

/* Odwrotna DCT: 64 wspolczynniki (po kwantyzacji odwroconej) -> blok pikseli */
static void jidct(struct jcomp *c, const int32_t coef[64], int bx, int by)
{
    int64_t tmp[64];
    for (int v = 0; v < 8; v++)                              /* najpierw wiersze... */
        for (int x = 0; x < 8; x++) {
            int64_t sum = 0;
            for (int u = 0; u < 8; u++)
                sum += (int64_t)coef[v * 8 + u] * idct_table[x][u];
            tmp[v * 8 + x] = sum;
        }
    for (int x = 0; x < 8; x++)                              /* ...potem kolumny */
        for (int y = 0; y < 8; y++) {
            int64_t sum = 0;
            for (int v = 0; v < 8; v++)
                sum += tmp[v * 8 + x] * idct_table[y][v];
            int val = (int)((sum + (1 << 23)) >> 24) + 128;
            int px = bx * 8 + x, py = by * 8 + y;
            if (px < c->pw && py < c->ph)
                c->plane[py * c->pw + px] = (uint8_t)(val < 0 ? 0 : val > 255 ? 255 : val);
        }
}

/* ---- JPEG progresywny: jeden blok w jednym przebiegu ---- */
static int jprog_block(struct jpeg *j, struct jcomp *c, int bx, int by)
{
    if (bx >= c->bw || by >= c->bh)
        return 0;
    int16_t *data = c->coefs + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64;
    if (j->ss == 0) {                                        /* wspolczynnik DC (srednia jasnosc bloku) */
        if (j->ah == 0) {
            int t = jdecode(j, &j->dc[c->td]);
            if (t < 0 || t > 16)
                return -1;
            c->dcpred += extend(jbits(j, t), t);
            data[0] = (int16_t)(c->dcpred * (1 << j->al));
        } else if (jbit(j)) {
            data[0] = (int16_t)(data[0] | (1 << j->al));     /* kolejny, dokladniejszy bit */
        }
        return 0;
    }
    const struct jhuff *hac = &j->ac[c->ta];
    if (j->ah == 0) {                                        /* AC - pierwsze przyblizenie */
        if (j->eobrun) {
            j->eobrun--;
            return 0;
        }
        for (int k = j->ss; k <= j->se;) {
            int rs = jdecode(j, hac);
            if (rs < 0)
                return -1;
            int r = rs >> 4, sz = rs & 15;
            if (sz == 0) {
                if (r < 15) {                                /* "kilka blokow bez niczego" */
                    j->eobrun = (1 << r) - 1;
                    if (r)
                        j->eobrun += jbits(j, r);
                    break;
                }
                k += 16;
            } else {
                k += r;
                if (k > 63)
                    return -1;
                data[zigzag[k++]] = (int16_t)(extend(jbits(j, sz), sz) * (1 << j->al));
            }
        }
        return 0;
    }
    /* AC - doprecyzowanie: po jednym bicie do juz znanych wspolczynnikow */
    int bit = 1 << j->al;
    if (j->eobrun) {
        j->eobrun--;
        for (int k = j->ss; k <= j->se; k++) {
            int16_t *pc = &data[zigzag[k]];
            if (*pc && jbit(j) && !(*pc & bit))
                *pc = (int16_t)(*pc > 0 ? *pc + bit : *pc - bit);
        }
        return 0;
    }
    int k = j->ss;
    do {
        int rs = jdecode(j, hac);
        if (rs < 0)
            return -1;
        int r = rs >> 4, sz = rs & 15;
        if (sz == 0) {
            if (r < 15) {
                j->eobrun = (1 << r) - 1;
                if (r)
                    j->eobrun += jbits(j, r);
                r = 64;                                      /* do konca bloku tylko poprawki */
            }
        } else {
            if (sz != 1)
                return -1;
            sz = jbit(j) ? bit : -bit;
        }
        while (k <= j->se) {
            int16_t *pc = &data[zigzag[k++]];
            if (*pc) {
                if (jbit(j) && !(*pc & bit))
                    *pc = (int16_t)(*pc > 0 ? *pc + bit : *pc - bit);
            } else {
                if (r == 0) {
                    *pc = (int16_t)sz;
                    break;
                }
                r--;
            }
        }
    } while (k <= j->se);
    return 0;
}

static int jany_block(struct jpeg *j, struct jcomp *c, int bx, int by)
{
    return j->progressive ? jprog_block(j, c, bx, by) : jblock(j, c, bx, by);
}

static void jreset(struct jpeg *j)
{
    j->bitcnt = 0;
    j->marker = 0;
    /* przeskocz znacznik RSTn */
    if (j->pos + 1 < j->len && j->d[j->pos] == 0xFF && j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7)
        j->pos += 2;
    for (int i = 0; i < j->nc; i++)
        j->c[i].dcpred = 0;
    j->eobrun = 0;
}

static int jscan(struct jpeg *j, const uint8_t *s, unsigned slen)
{
    int ns = s[0];
    if (ns < 1 || ns > 3 || slen < (unsigned)(1 + ns * 2 + 3))
        return -1;
    struct jcomp *sc[3];
    for (int i = 0; i < ns; i++) {
        int id = s[1 + i * 2];
        sc[i] = 0;
        for (int k = 0; k < j->nc; k++)
            if (j->c[k].id == id)
                sc[i] = &j->c[k];
        if (!sc[i])
            return -1;
        sc[i]->td = s[2 + i * 2] >> 4;
        sc[i]->ta = s[2 + i * 2] & 15;
        sc[i]->dcpred = 0;
    }
    j->ss = s[1 + ns * 2];
    j->se = s[2 + ns * 2];
    j->ah = s[3 + ns * 2] >> 4;
    j->al = s[3 + ns * 2] & 15;
    j->eobrun = 0;
    if (!j->progressive) {
        j->ss = 0;
        j->se = 63;
        j->ah = j->al = 0;
    } else if (j->se > 63 || j->ss > j->se || (j->ss > 0 && ns != 1)) {
        return -1;
    }
    for (int i = 0; i < ns; i++) {
        int need_dc = j->ss == 0 && j->ah == 0, need_ac = j->se > 0;
        if (sc[i]->td > 3 || sc[i]->ta > 3 || (need_dc && !j->dc[sc[i]->td].ready) ||
            (need_ac && !j->ac[sc[i]->ta].ready))
            return -1;
    }
    j->bitcnt = 0;
    j->marker = 0;
    int mcus = 0;
    if (ns == 1) {                                           /* jeden kolor osobno: bloki po kolei */
        struct jcomp *c = sc[0];
        int bw = ((j->w * c->h + j->hmax - 1) / j->hmax + 7) / 8;
        int bh = ((j->h * c->v + j->vmax - 1) / j->vmax + 7) / 8;
        for (int by = 0; by < bh; by++, BREATHE())
            for (int bx = 0; bx < bw; bx++) {
                if (j->restart && mcus && mcus % j->restart == 0)
                    jreset(j);
                if (jany_block(j, c, bx, by))
                    return -1;
                mcus++;
            }
    } else {                                                 /* MCU: bloki wszystkich kolorow razem */
        int mx = (j->w + 8 * j->hmax - 1) / (8 * j->hmax), my = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
        for (int y = 0; y < my; y++, BREATHE())
            for (int x = 0; x < mx; x++) {
                if (j->restart && mcus && mcus % j->restart == 0)
                    jreset(j);
                for (int i = 0; i < ns; i++)
                    for (int v = 0; v < sc[i]->v; v++)
                        for (int h = 0; h < sc[i]->h; h++)
                            if (jany_block(j, sc[i], x * sc[i]->h + h, y * sc[i]->v + v))
                                return -1;
                mcus++;
            }
    }
    /* Za danymi: szukamy nastepnego znacznika */
    while (j->pos + 1 < j->len && !(j->d[j->pos] == 0xFF && j->d[j->pos + 1] != 0 &&
                                    !(j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7)))
        j->pos++;
    return 0;
}

static int jpeg_decode(const uint8_t *d, unsigned len, struct image *img)
{
    struct jpeg *j = kzalloc(sizeof(*j));
    if (!j)
        return IMG_ENOMEM;
    j->d = d;
    j->len = len;
    j->pos = 2;
    int err = IMG_ECORRUPT, have_frame = 0, scanned = 0;
    while (j->pos + 4 <= len) {
        if (d[j->pos] != 0xFF) {
            j->pos++;
            continue;
        }
        uint8_t m = d[j->pos + 1];
        if (m == 0xFF) {                                    /* wypelnienie */
            j->pos++;
            continue;
        }
        if (m == 0xD9)                                      /* EOI - koniec obrazka */
            break;
        unsigned seglen = be16(d + j->pos + 2);
        const uint8_t *s = d + j->pos + 4;
        if (seglen < 2 || j->pos + 2 + seglen > len)
            break;
        unsigned slen = seglen - 2;
        j->pos += 2 + seglen;
        if (m == 0xDB) {                                    /* tablice kwantyzacji */
            for (unsigned p = 0; p < slen;) {
                int pq = s[p] >> 4, tq = s[p] & 15;
                if (tq > 3 || p + 1 + 64 * (pq + 1) > slen)
                    goto out;
                for (int i = 0; i < 64; i++)
                    j->qt[tq][zigzag[i]] = pq ? be16(s + p + 1 + i * 2) : s[p + 1 + i];
                p += 1 + 64 * (unsigned)(pq + 1);
            }
        } else if (m == 0xC4) {                             /* tablice Huffmana */
            for (unsigned p = 0; p + 17 <= slen;) {
                int tc = s[p] >> 4, th = s[p] & 15;
                if (th > 3 || tc > 1)
                    goto out;
                struct jhuff *h = tc ? &j->ac[th] : &j->dc[th];
                int total = 0;
                for (int l = 1; l <= 16; l++)
                    total += s[p + l];
                if (total > 256 || p + 17 + (unsigned)total > slen)
                    goto out;
                memcpy(h->vals, s + p + 17, (size_t)total);
                int code = 0, k = 0;
                for (int l = 1; l <= 16; l++) {
                    int n = s[p + l];
                    h->valptr[l] = k;
                    h->mincode[l] = code;
                    code += n;
                    k += n;
                    h->maxcode[l] = n ? code - 1 : -1;
                    code <<= 1;
                }
                h->ready = 1;
                p += 17 + (unsigned)total;
            }
        } else if (m == 0xDD && slen >= 2) {                /* co ile MCU znacznik RST */
            j->restart = be16(s);
        } else if (m == 0xC0 || m == 0xC1 || m == 0xC2) {   /* ramka: "baseline" albo progresywna */
            j->progressive = m == 0xC2;
            if (slen < 6 || s[0] != 8)
                goto out;
            j->h = be16(s + 1);
            j->w = be16(s + 3);
            j->nc = s[5];
            if ((j->nc != 1 && j->nc != 3) || slen < 6 + 3u * (unsigned)j->nc)
                goto out;
            j->hmax = j->vmax = 1;
            for (int i = 0; i < j->nc; i++) {
                j->c[i].id = s[6 + i * 3];
                j->c[i].h = s[7 + i * 3] >> 4;
                j->c[i].v = s[7 + i * 3] & 15;
                j->c[i].tq = s[8 + i * 3] & 3;
                if (j->c[i].h < 1 || j->c[i].h > 4 || j->c[i].v < 1 || j->c[i].v > 4)
                    goto out;
                if (j->c[i].h > j->hmax) j->hmax = j->c[i].h;
                if (j->c[i].v > j->vmax) j->vmax = j->c[i].v;
            }
            if (j->w <= 0 || j->h <= 0 || (uint64_t)j->w * (uint64_t)j->h > MAX_PIXELS) {
                err = IMG_ETOOBIG;
                goto out;
            }
            int mx = (j->w + 8 * j->hmax - 1) / (8 * j->hmax), my = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
            for (int i = 0; i < j->nc; i++) {
                j->c[i].pw = mx * j->c[i].h * 8;
                j->c[i].ph = my * j->c[i].v * 8;
                j->c[i].plane = kzalloc((size_t)j->c[i].pw * (size_t)j->c[i].ph);
                j->c[i].bw = j->c[i].pw / 8;
                j->c[i].bh = j->c[i].ph / 8;
                if (j->progressive)
                    j->c[i].coefs = kzalloc((size_t)j->c[i].bw * (size_t)j->c[i].bh * 64 * 2);
                if (!j->c[i].plane || (j->progressive && !j->c[i].coefs)) {
                    err = IMG_ENOMEM;
                    goto out;
                }
            }
            have_frame = 1;
        } else if (m == 0xC6 || m == 0xCA || m == 0xC3 || m == 0xC9) {
            err = IMG_EFORMAT;                              /* arytmetyczny / bezstratny - rzadkie */
            goto out;
        } else if (m == 0xDA) {                             /* dane obrazu */
            if (!have_frame || jscan(j, s, slen))
                goto out;
            scanned = 1;
        }
    }
    if (!scanned)
        goto out;
    if (j->progressive) {                                   /* wszystkie przebiegi zebrane - teraz piksele */
        for (int i = 0; i < j->nc; i++) {
            struct jcomp *c = &j->c[i];
            const uint16_t *q = j->qt[c->tq];
            for (int by = 0; by < c->bh; by++, BREATHE())
                for (int bx = 0; bx < c->bw; bx++) {
                    const int16_t *data = c->coefs + ((size_t)by * (size_t)c->bw + (size_t)bx) * 64;
                    int32_t coef[64];
                    for (int k = 0; k < 64; k++)
                        coef[k] = data[k] * q[k];
                    jidct(c, coef, bx, by);
                }
        }
    }
    err = alloc_image(img, j->w, j->h);
    if (err)
        goto out;
    for (int y = 0; y < j->h; y++)
        for (int x = 0; x < j->w; x++) {
            const struct jcomp *c0 = &j->c[0];
            int Y = c0->plane[(y * c0->v / j->vmax) * c0->pw + x * c0->h / j->hmax];
            if (j->nc == 1) {
                img->px[y * j->w + x] = rgb(Y, Y, Y);
                continue;
            }
            const struct jcomp *c1 = &j->c[1], *c2 = &j->c[2];
            int cb = c1->plane[(y * c1->v / j->vmax) * c1->pw + x * c1->h / j->hmax] - 128;
            int cr = c2->plane[(y * c2->v / j->vmax) * c2->pw + x * c2->h / j->hmax] - 128;
            int r = Y + ((91881 * cr) >> 16);                       /* YCbCr -> RGB */
            int g = Y - ((22554 * cb + 46802 * cr) >> 16);
            int b = Y + ((116130 * cb) >> 16);
            img->px[y * j->w + x] = rgb(r < 0 ? 0 : r > 255 ? 255 : r, g < 0 ? 0 : g > 255 ? 255 : g,
                                        b < 0 ? 0 : b > 255 ? 255 : b);
        }
    err = 0;
out:
    for (int i = 0; i < 3; i++) {
        kfree(j->c[i].plane);
        kfree(j->c[i].coefs);
    }
    kfree(j);
    return err;
}

/* ============================ zmniejszanie ============================ */

/* Kazdy piksel wyniku = srednia z prostokata pikseli oryginalu */
static int shrink(struct image *img, int nw, int nh)
{
    uint32_t *out = kmalloc((size_t)nw * (size_t)nh * 4);
    if (!out)
        return IMG_ENOMEM;
    for (int dy = 0; dy < nh; dy++) {
        if ((dy & 7) == 7)
            BREATHE();
        int y0 = dy * img->h / nh, y1 = (dy + 1) * img->h / nh;
        if (y1 <= y0) y1 = y0 + 1;
        for (int dx = 0; dx < nw; dx++) {
            int x0 = dx * img->w / nw, x1 = (dx + 1) * img->w / nw;
            if (x1 <= x0) x1 = x0 + 1;
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int y = y0; y < y1; y++)
                for (int x = x0; x < x1; x++) {
                    uint32_t p = img->px[y * img->w + x];
                    r += p >> 16 & 255;
                    g += p >> 8 & 255;
                    b += p & 255;
                    n++;
                }
            out[dy * nw + dx] = rgb((int)(r / n), (int)(g / n), (int)(b / n));
        }
    }
    kfree(img->px);
    img->px = out;
    img->w = nw;
    img->h = nh;
    return 0;
}

int image_decode(const uint8_t *d, unsigned len, struct image *img, int max_w, int max_h)
{
    img->px = 0;
    img->w = img->h = 0;
    int err;
    if (len >= 8 && memcmp(d, "\x89PNG", 4) == 0)
        err = png_decode(d, len, img);
    else if (len >= 6 && memcmp(d, "GIF8", 4) == 0)
        err = gif_decode(d, len, img);
    else if (len >= 4 && d[0] == 0xFF && d[1] == 0xD8)
        err = jpeg_decode(d, len, img);
    else
        err = IMG_EFORMAT;
    if (err) {
        image_free(img);
        return err;
    }
    if (img->w > max_w || img->h > max_h) {
        int nw = max_w, nh = (int)((int64_t)img->h * max_w / img->w);
        if (nh > max_h) {
            nh = max_h;
            nw = (int)((int64_t)img->w * max_h / img->h);
        }
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        err = shrink(img, nw, nh);
        if (err)
            image_free(img);
    }
    return err;
}

/* ============================ base64 ============================ */

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

unsigned base64_decode(const char *in, uint8_t **out)
{
    unsigned n = (unsigned)strlen(in);
    uint8_t *o = kmalloc(n / 4 * 3 + 4);
    if (!o)
        return 0;
    unsigned len = 0, acc = 0;
    int bitsn = 0;
    for (unsigned i = 0; i < n; i++) {
        int v = b64_value(in[i]);
        if (v < 0)
            continue;                      /* '=' na koncu, spacje, nowe linie */
        acc = acc << 6 | (unsigned)v;
        bitsn += 6;
        if (bitsn >= 8) {
            bitsn -= 8;
            o[len++] = (uint8_t)(acc >> bitsn);
        }
    }
    *out = o;
    return len;
}

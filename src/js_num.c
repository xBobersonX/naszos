/* Liczby JavaScriptu: zamiana liczba <-> tekst i funkcje Math.
 *
 * Liczba w JS to "double": 53 bity cyfr (mantysa) i wykladnik potegi 2.
 * Zamiana na tekst musi dac NAJKROTSZY zapis dziesietny, ktory po
 * wczytaniu z powrotem daje dokladnie te sama liczbe (0.1 + 0.2 wypisuje
 * sie jako 0.30000000000000004, a 0.1 jako 0.1). Liczymy to dokladnie na
 * duzych liczbach calkowitych (algorytm Burgera i Dybviga) - zwykle
 * mnozenie double gubiloby ostatnie cyfry.
 *
 * sin, cos, exp, log, atan: wzory (wielomiany) z biblioteki fdlibm - po
 * sprowadzeniu argumentu do malego przedzialu wielomian daje wynik z
 * dokladnoscia do ostatniego bitu. */

#include "js_int.h"
#include "string.h"

static inline uint64_t dbits(double d)
{
    union { double d; uint64_t u; } x;
    x.d = d;
    return x.u;
}

static inline double bitsd(uint64_t u)
{
    union { double d; uint64_t u; } x;
    x.u = u;
    return x.d;
}

const double js_nan = __builtin_nan("");
const double js_inf = __builtin_inf();

/* ---------- duze liczby calkowite (do dokladnych zamian) ---------- */

#define BN_WORDS 56

typedef struct {
    int n;                           /* uzywane slowa */
    uint32_t d[BN_WORDS];            /* od najmniej znaczacego */
} bn;

static void bn_set(bn *a, uint64_t v)
{
    a->n = 0;
    while (v) {
        a->d[a->n++] = (uint32_t)v;
        v >>= 32;
    }
}

static void bn_trim(bn *a)
{
    while (a->n && !a->d[a->n - 1])
        a->n--;
}

static void bn_shl(bn *a, int bits)
{
    if (!a->n || bits <= 0)
        return;
    int words = bits / 32, sh = bits % 32;
    int n = a->n + words + 1;
    if (n > BN_WORDS)
        n = BN_WORDS;
    for (int i = n - 1; i >= 0; i--) {
        int src = i - words;
        uint64_t v = 0;
        if (src >= 0 && src < a->n)
            v = (uint64_t)a->d[src] << sh;
        if (sh && src - 1 >= 0 && src - 1 < a->n)
            v |= a->d[src - 1] >> (32 - sh);
        a->d[i] = (uint32_t)v;
    }
    a->n = n;
    bn_trim(a);
}

static void bn_mul_small(bn *a, uint32_t m)
{
    uint64_t carry = 0;
    for (int i = 0; i < a->n; i++) {
        uint64_t v = (uint64_t)a->d[i] * m + carry;
        a->d[i] = (uint32_t)v;
        carry = v >> 32;
    }
    if (carry && a->n < BN_WORDS)
        a->d[a->n++] = (uint32_t)carry;
}

static void bn_mul_pow10(bn *a, int k)
{
    while (k >= 9) {
        bn_mul_small(a, 1000000000u);
        k -= 9;
    }
    static const uint32_t p10[9] = { 1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000 };
    if (k > 0)
        bn_mul_small(a, p10[k]);
}

static int bn_cmp(const bn *a, const bn *b)
{
    if (a->n != b->n)
        return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i])
            return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

static void bn_add(bn *r, const bn *a, const bn *b)
{
    int n = a->n > b->n ? a->n : b->n;
    uint64_t carry = 0;
    for (int i = 0; i < n; i++) {
        uint64_t v = carry;
        if (i < a->n) v += a->d[i];
        if (i < b->n) v += b->d[i];
        r->d[i] = (uint32_t)v;
        carry = v >> 32;
    }
    r->n = n;
    if (carry && n < BN_WORDS)
        r->d[r->n++] = (uint32_t)carry;
}

/* a -= b (a >= b) */
static void bn_sub(bn *a, const bn *b)
{
    int64_t borrow = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t v = (int64_t)a->d[i] - borrow - (i < b->n ? (int64_t)b->d[i] : 0);
        borrow = v < 0;
        a->d[i] = (uint32_t)(v + (borrow ? ((int64_t)1 << 32) : 0));
    }
    bn_trim(a);
}

/* a /= m, zwraca reszte */
static uint32_t bn_div_small(bn *a, uint32_t m)
{
    uint64_t rem = 0;
    for (int i = a->n - 1; i >= 0; i--) {
        uint64_t v = rem << 32 | a->d[i];
        a->d[i] = (uint32_t)(v / m);
        rem = v % m;
    }
    bn_trim(a);
    return (uint32_t)rem;
}

/* Cyfry dziesietne liczby a (niszczy a). Zwraca dlugosc */
static int bn_to_dec(bn *a, char *out, int size)
{
    char tmp[400];
    int n = 0;
    if (!a->n)
        tmp[n++] = '0';
    while (a->n && n < (int)sizeof(tmp) - 9) {
        uint32_t chunk = bn_div_small(a, 1000000000u);
        for (int i = 0; i < 9; i++) {
            tmp[n++] = (char)('0' + chunk % 10);
            chunk /= 10;
            if (!a->n && !chunk)
                break;
        }
    }
    int len = 0;
    for (int i = n - 1; i >= 0 && len < size - 1; i--)
        out[len++] = tmp[i];
    out[len] = '\0';
    return len;
}

/* Rozklad liczby dodatniej skonczonej: d = f * 2^e */
static void decompose(double d, uint64_t *f, int *e, int *be)
{
    uint64_t b = dbits(d);
    int exp_bits = (int)((b >> 52) & 0x7FF);
    uint64_t mant = b & ((1ULL << 52) - 1);
    if (exp_bits == 0) {
        *f = mant;
        *e = -1074;
    } else {
        *f = mant | (1ULL << 52);
        *e = exp_bits - 1075;
    }
    *be = exp_bits;
}

static int bitlen(uint64_t v)
{
    int n = 0;
    while (v) {
        n++;
        v >>= 1;
    }
    return n;
}

/* Najkrotsze cyfry: d = 0.cyfry * 10^k. Zwraca liczbe cyfr */
static int shortest(double d, char *digits, int *kout)
{
    uint64_t f;
    int e, be;
    decompose(d, &f, &e, &be);
    int even = (f & 1) == 0;
    bn r, s, mp, mm;
    int unequal = 0;                               /* m+ != m- */
    if (e >= 0) {
        if (f != (1ULL << 52)) {
            bn_set(&r, f); bn_shl(&r, e + 1);
            bn_set(&s, 2);
            bn_set(&mp, 1); bn_shl(&mp, e);
        } else {
            bn_set(&r, f); bn_shl(&r, e + 2);
            bn_set(&s, 4);
            bn_set(&mp, 1); bn_shl(&mp, e + 1);
            unequal = 1;
        }
    } else {
        if (be <= 1 || f != (1ULL << 52)) {
            bn_set(&r, f); bn_shl(&r, 1);
            bn_set(&s, 1); bn_shl(&s, 1 - e);
            bn_set(&mp, 1);
        } else {
            bn_set(&r, f); bn_shl(&r, 2);
            bn_set(&s, 1); bn_shl(&s, 2 - e);
            bn_set(&mp, 2);
            unequal = 1;
        }
    }
    bn_set(&mm, 1);
    if (!unequal)
        mm = mp;
    else if (e >= 0)
        bn_shl(&mm, e);
    /* k = ceil(log10(d)) - oszacowanie (moze byc o 1 za male) */
    double est = (e + bitlen(f) - 1) * 0.30102999566398114 - 1e-10;
    int k = (int)est;
    if (est > k)
        k++;
    if (k >= 0) {
        bn_mul_pow10(&s, k);
    } else {
        bn_mul_pow10(&r, -k);
        bn_mul_pow10(&mp, -k);
        bn_mul_pow10(&mm, -k);
    }
    bn t;
    bn_add(&t, &r, &mp);
    int c = bn_cmp(&t, &s);
    if (even ? c >= 0 : c > 0) {
        k++;
        bn_mul_small(&s, 10);
    }
    int n = 0;
    for (;;) {
        bn_mul_small(&r, 10);
        bn_mul_small(&mp, 10);
        bn_mul_small(&mm, 10);
        int dg = 0;
        while (bn_cmp(&r, &s) >= 0) {
            bn_sub(&r, &s);
            dg++;
        }
        int c1 = bn_cmp(&r, &mm);
        int tc1 = even ? c1 <= 0 : c1 < 0;
        bn_add(&t, &r, &mp);
        int c2 = bn_cmp(&t, &s);
        int tc2 = even ? c2 >= 0 : c2 > 0;
        if (!tc1 && !tc2) {
            digits[n++] = (char)('0' + dg);
            if (n >= 20)
                break;
            continue;
        }
        if (tc1 && !tc2) {
            digits[n++] = (char)('0' + dg);
        } else if (!tc1 && tc2) {
            digits[n++] = (char)('0' + dg + 1);
        } else {
            bn two;
            two = r;
            bn_shl(&two, 1);
            int c3 = bn_cmp(&two, &s);
            digits[n++] = (char)('0' + (c3 < 0 ? dg : c3 > 0 ? dg + 1 : (dg % 2 ? dg + 1 : dg)));
        }
        break;
    }
    digits[n] = '\0';
    *kout = k;
    return n;
}

int js_dtoa(double d, char *out)
{
    char *o = out;
    if (js_isnan(d)) {
        strcpy(out, "NaN");
        return 3;
    }
    if (d == 0) {
        strcpy(out, "0");
        return 1;
    }
    if (d < 0) {
        *o++ = '-';
        d = -d;
    }
    if (js_isinf(d)) {
        strcpy(o, "Infinity");
        return (int)(o - out) + 8;
    }
    /* Liczby calkowite do 2^53 - szybko */
    if (d < 9007199254740992.0 && d == (double)(uint64_t)d) {
        uint64_t v = (uint64_t)d;
        char tmp[24];
        int n = 0;
        while (v) {
            tmp[n++] = (char)('0' + v % 10);
            v /= 10;
        }
        while (n)
            *o++ = tmp[--n];
        *o = '\0';
        return (int)(o - out);
    }
    char dig[24];
    int k;
    int nd = shortest(d, dig, &k);
    if (nd <= k && k <= 21) {
        memcpy(o, dig, (size_t)nd);
        o += nd;
        for (int i = nd; i < k; i++)
            *o++ = '0';
    } else if (0 < k && k <= 21) {
        memcpy(o, dig, (size_t)k);
        o += k;
        *o++ = '.';
        memcpy(o, dig + k, (size_t)(nd - k));
        o += nd - k;
    } else if (-6 < k && k <= 0) {
        *o++ = '0';
        *o++ = '.';
        for (int i = 0; i < -k; i++)
            *o++ = '0';
        memcpy(o, dig, (size_t)nd);
        o += nd;
    } else {
        int ex = k - 1;
        *o++ = dig[0];
        if (nd > 1) {
            *o++ = '.';
            memcpy(o, dig + 1, (size_t)(nd - 1));
            o += nd - 1;
        }
        *o++ = 'e';
        *o++ = ex < 0 ? '-' : '+';
        if (ex < 0)
            ex = -ex;
        char tmp[8];
        int n = 0;
        do {
            tmp[n++] = (char)('0' + ex % 10);
            ex /= 10;
        } while (ex);
        while (n)
            *o++ = tmp[--n];
    }
    *o = '\0';
    return (int)(o - out);
}

/* ---------- tekst -> liczba ---------- */

static const double pow10_tab[23] = {
    1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
    1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

static double next_up(double x)   { return bitsd(dbits(x) + 1); }     /* x > 0 */
static double next_down(double x) { return bitsd(dbits(x) - 1); }

/* Dokladne porownanie D * 10^E z (2m+1) * 2^(e2-1) (polowa drogi do nastepnej liczby) */
static int cmp_halfway(uint64_t D, int E, uint64_t m2p1, int e2m1)
{
    bn a, b;
    bn_set(&a, D);
    bn_set(&b, m2p1);
    if (E >= 0)
        bn_mul_pow10(&a, E);
    else
        bn_mul_pow10(&b, -E);
    if (e2m1 >= 0)
        bn_shl(&b, e2m1);
    else
        bn_shl(&a, -e2m1);
    return bn_cmp(&a, &b);
}

static double scale10(double x, int e)
{
    while (e > 22) {
        x *= 1e22;
        e -= 22;
    }
    while (e < -22) {
        x /= 1e22;
        e += 22;
    }
    return e >= 0 ? x * pow10_tab[e] : x / pow10_tab[-e];
}

/* Liczba D * 10^E zaokraglona poprawnie do double */
static double make_double(uint64_t D, int E)
{
    if (!D)
        return 0;
    if (E > 330)
        return js_inf;
    if (E < -360)
        return 0;
    if (D < (1ULL << 53) && E >= -22 && E <= 22)
        return E >= 0 ? (double)D * pow10_tab[E] : (double)D / pow10_tab[-E];
    double x = scale10((double)D, E);
    if (js_isinf(x))
        x = bitsd(0x7FEFFFFFFFFFFFFFULL);
    if (x == 0)
        x = bitsd(1);
    for (int iter = 0; iter < 64; iter++) {
        uint64_t f;
        int e, be;
        decompose(x, &f, &e, &be);
        /* gorna polowa: (2f+1) * 2^(e-1) */
        int c = cmp_halfway(D, E, 2 * f + 1, e - 1);
        if (c > 0 || (c == 0 && (f & 1))) {
            if (dbits(x) == 0x7FEFFFFFFFFFFFFFULL)
                return c > 0 ? js_inf : x;
            x = next_up(x);
            continue;
        }
        /* dolna polowa: (2f-1) * 2^(e-1); przy f = 2^52 odstep w dol jest dwa razy mniejszy */
        int c2;
        if (f == (1ULL << 52) && be > 1)
            c2 = cmp_halfway(D, E, 4 * f - 1, e - 2);
        else
            c2 = cmp_halfway(D, E, 2 * f - 1, e - 1);
        if (c2 < 0 || (c2 == 0 && (f & 1))) {
            if (dbits(x) == 1)
                return c2 < 0 ? 0 : x;
            x = next_down(x);
            continue;
        }
        break;
    }
    return x;
}

double js_strtod(const char *s, const char **end)
{
    int neg = 0;
    if (*s == '-' || *s == '+')
        neg = *s++ == '-';
    uint64_t D = 0;
    int E = 0, digits = 0, any = 0, sticky = 0;
    while (*s >= '0' && *s <= '9') {
        any = 1;
        if (digits < 19) {
            D = D * 10 + (uint64_t)(*s - '0');
            if (D)
                digits++;
        } else {
            E++;
            if (*s != '0')
                sticky = 1;
        }
        s++;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            any = 1;
            if (digits < 19) {
                D = D * 10 + (uint64_t)(*s - '0');
                if (D)
                    digits++;
                E--;
            } else if (*s != '0') {
                sticky = 1;
            }
            s++;
        }
    }
    if (!any) {
        if (end)
            *end = 0;
        return js_nan;
    }
    if (*s == 'e' || *s == 'E') {
        const char *q = s + 1;
        int eneg = 0;
        if (*q == '+' || *q == '-')
            eneg = *q++ == '-';
        if (*q >= '0' && *q <= '9') {
            int ev = 0;
            while (*q >= '0' && *q <= '9') {
                if (ev < 100000)
                    ev = ev * 10 + (*q - '0');
                q++;
            }
            E += eneg ? -ev : ev;
            s = q;
        }
    }
    if (end)
        *end = s;
    if (sticky && D < (1ULL << 60)) {                  /* dalsze cyfry: jak "...5" na koncu */
        D = D * 10 + 5;
        E--;
    }
    double r = make_double(D, E);
    return neg ? -r : r;
}

/* Number("tekst"): odstepy dookola, 0x, Infinity, pusty = 0 */
double js_str_to_num(const jstr *str)
{
    uint32_t i = 0, n = str->len;
    while (i < n && js_is_space(str->c[i]))
        i++;
    while (n > i && js_is_space(str->c[n - 1]))
        n--;
    if (i == n)
        return 0;
    char buf[400];
    uint32_t len = n - i;
    if (len >= sizeof(buf))
        return js_nan;
    for (uint32_t k = 0; k < len; k++) {
        uint16_t c = str->c[i + k];
        if (c >= 128)
            return js_nan;
        buf[k] = (char)c;
    }
    buf[len] = '\0';
    const char *p = buf;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X' || p[1] == 'o' || p[1] == 'O' || p[1] == 'b' || p[1] == 'B')) {
        int radix = (p[1] | 0x20) == 'x' ? 16 : (p[1] | 0x20) == 'o' ? 8 : 2;
        double v = 0;
        p += 2;
        if (!*p)
            return js_nan;
        for (; *p; p++) {
            int d = *p >= '0' && *p <= '9' ? *p - '0' : (*p | 0x20) >= 'a' && (*p | 0x20) <= 'z' ? (*p | 0x20) - 'a' + 10 : 99;
            if (d >= radix)
                return js_nan;
            v = v * radix + d;
        }
        return v;
    }
    int neg = 0;
    const char *q = p;
    if (*q == '+' || *q == '-')
        neg = *q++ == '-';
    if (strcmp(q, "Infinity") == 0)
        return neg ? -js_inf : js_inf;
    const char *end;
    if ((*q < '0' || *q > '9') && *q != '.')
        return js_nan;
    double v = js_strtod(p, &end);
    if (!end || *end)
        return js_nan;
    return v;
}

/* ---------- toFixed, toPrecision, toExponential ---------- */

/* round(|d| * 10^s) jako tekst cyfr (polowki w gore): licznik / mianownik
 * na duzych liczbach i dzielenie "pod kreska", bit po bicie */
static int scaled_digits(double d, int s, char *out, int size)
{
    uint64_t f;
    int e, be;
    decompose(d, &f, &e, &be);
    bn num, den;
    bn_set(&num, f);
    bn_set(&den, 1);
    if (s >= 0)
        bn_mul_pow10(&num, s);
    else
        bn_mul_pow10(&den, -s);
    if (e >= 0)
        bn_shl(&num, e);
    else
        bn_shl(&den, -e);
    /* (2 * num + den) / (2 * den) = zaokraglenie polowek w gore */
    bn_shl(&num, 1);
    bn_add(&num, &num, &den);
    bn_shl(&den, 1);
    bn q, rem;
    bn_set(&q, 0);
    bn_set(&rem, 0);
    for (int i = num.n * 32 - 1; i >= 0; i--) {
        bn_shl(&rem, 1);
        if (num.d[i / 32] >> (i % 32) & 1) {
            if (!rem.n) {
                rem.n = 1;
                rem.d[0] = 0;
            }
            rem.d[0] |= 1;
        }
        bn_shl(&q, 1);
        if (bn_cmp(&rem, &den) >= 0) {
            bn_sub(&rem, &den);
            if (!q.n) {
                q.n = 1;
                q.d[0] = 0;
            }
            q.d[0] |= 1;
        }
    }
    bn_trim(&q);
    return bn_to_dec(&q, out, size);
}

int js_fixed(double d, int digits, char *out, unsigned size)
{
    if (!js_isfinite(d) || d >= 1e21 || d <= -1e21) {
        char tmp[40];
        js_dtoa(d, tmp);
        return ksnprintf(out, size, "%s", tmp);
    }
    int neg = d < 0;
    if (neg)
        d = -d;
    char dig[400];
    int n = scaled_digits(d, digits, dig, sizeof(dig));
    char *o = out, *end = out + size - 1;
    if (neg)
        *o++ = '-';
    if (digits == 0) {
        for (int i = 0; i < n && o < end; i++)
            *o++ = dig[i];
    } else {
        int intlen = n - digits;
        if (intlen <= 0) {
            *o++ = '0';
            *o++ = '.';
            for (int i = 0; i < -intlen && o < end; i++)
                *o++ = '0';
            for (int i = 0; i < n && o < end; i++)
                *o++ = dig[i];
        } else {
            for (int i = 0; i < intlen && o < end; i++)
                *o++ = dig[i];
            *o++ = '.';
            for (int i = intlen; i < n && o < end; i++)
                *o++ = dig[i];
        }
    }
    *o = '\0';
    /* -0.00 -> "-0.00" jak w JS (dla liczb ujemnych), ale (-0).toFixed(2) = "0.00" */
    return (int)(o - out);
}

/* Cyfry: dokladnie p cyfr znaczacych; *e10 = wykladnik pierwszej cyfry */
static void precise_digits(double d, int p, char *dig, int *e10)
{
    uint64_t f;
    int be, e2;
    decompose(d, &f, &e2, &be);
    double est = (e2 + bitlen(f) - 1) * 0.30102999566398114;
    int e = (int)est;
    if (est < 0 && est != e)
        e--;
    for (int tries = 0; tries < 4; tries++) {
        int n = scaled_digits(d, p - 1 - e, dig, 400);
        if (n > p)
            e++;
        else if (n < p)
            e--;
        else
            break;
    }
    dig[p] = '\0';
    *e10 = e;
}

int js_precision(double d, int p, char *out, unsigned size)
{
    if (!js_isfinite(d)) {
        char tmp[40];
        js_dtoa(d, tmp);
        return ksnprintf(out, size, "%s", tmp);
    }
    char *o = out;
    if (d < 0) {
        *o++ = '-';
        d = -d;
        size--;
    }
    if (d == 0) {
        *o++ = '0';
        if (p > 1) {
            *o++ = '.';
            for (int i = 1; i < p; i++)
                *o++ = '0';
        }
        *o = '\0';
        return (int)(o - out);
    }
    char dig[400];
    int e;
    precise_digits(d, p, dig, &e);
    if (e < -6 || e >= p) {
        *o++ = dig[0];
        if (p > 1) {
            *o++ = '.';
            memcpy(o, dig + 1, (size_t)(p - 1));
            o += p - 1;
        }
        o += ksnprintf(o, 16, "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    } else if (e >= 0) {
        memcpy(o, dig, (size_t)(e + 1));
        o += e + 1;
        if (p > e + 1) {
            *o++ = '.';
            memcpy(o, dig + e + 1, (size_t)(p - e - 1));
            o += p - e - 1;
        }
    } else {
        *o++ = '0';
        *o++ = '.';
        for (int i = 0; i < -e - 1; i++)
            *o++ = '0';
        memcpy(o, dig, (size_t)p);
        o += p;
    }
    *o = '\0';
    return (int)(o - out);
}

int js_exponential(double d, int digits, char *out, unsigned size)
{
    if (!js_isfinite(d)) {
        char tmp[40];
        js_dtoa(d, tmp);
        return ksnprintf(out, size, "%s", tmp);
    }
    char *o = out;
    if (d < 0) {
        *o++ = '-';
        d = -d;
    }
    char dig[400];
    int e, p;
    if (d == 0) {
        e = 0;
        p = digits < 0 ? 1 : digits + 1;
        for (int i = 0; i < p; i++)
            dig[i] = '0';
        dig[p] = '\0';
    } else if (digits < 0) {
        int k;
        p = shortest(d, dig, &k);
        e = k - 1;
    } else {
        p = digits + 1;
        precise_digits(d, p, dig, &e);
    }
    *o++ = dig[0];
    if (p > 1) {
        *o++ = '.';
        memcpy(o, dig + 1, (size_t)(p - 1));
        o += p - 1;
    }
    o += ksnprintf(o, 16, "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    (void)size;
    return (int)(o - out);
}

int js_radix(double d, int radix, char *out, unsigned size)
{
    static const char digs[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    if (!js_isfinite(d) || radix == 10) {
        char tmp[40];
        js_dtoa(d, tmp);
        return ksnprintf(out, size, "%s", tmp);
    }
    char tmp[1200];
    int n = 0, neg = d < 0;
    if (neg)
        d = -d;
    double ip = js_floor(d), fp = d - ip;
    char rev[1100];
    int rn = 0;
    if (ip == 0)
        rev[rn++] = '0';
    while (ip >= 1 && rn < 1090) {
        double q = js_floor(ip / radix);
        int dg = (int)(ip - q * radix);
        if (dg < 0) dg = 0;
        if (dg >= radix) dg = radix - 1;
        rev[rn++] = digs[dg];
        ip = q;
    }
    if (neg)
        tmp[n++] = '-';
    while (rn)
        tmp[n++] = rev[--rn];
    if (fp > 0) {
        tmp[n++] = '.';
        /* tyle cyfr, ile trzeba, zeby odroznic od sasiednich liczb (jak V8) */
        double delta = 0.5 * (next_up(d) - d);
        if (delta <= 0)
            delta = 1e-300;
        int count = 0;
        do {
            fp *= radix;
            delta *= radix;
            int dg = (int)fp;
            tmp[n++] = digs[dg];
            fp -= dg;
            if (fp > 0.5 || (fp == 0.5 && (dg & 1))) {
                if (fp + delta > 1) {
                    /* zaokraglenie w gore */
                    for (;;) {
                        n--;
                        if (tmp[n] == '.') {
                            n++;
                            break;
                        }
                        int v = tmp[n] <= '9' ? tmp[n] - '0' : tmp[n] - 'a' + 10;
                        if (v + 1 < radix) {
                            tmp[n++] = digs[v + 1];
                            break;
                        }
                    }
                    break;
                }
            }
        } while (fp >= delta && ++count < 1070 && n < (int)sizeof(tmp) - 2);
    }
    tmp[n] = '\0';
    return ksnprintf(out, size, "%s", tmp);
}

/* ---------- Math ---------- */

double js_fabs(double x) { return bitsd(dbits(x) & ~(1ULL << 63)); }

double js_sqrt(double x)
{
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

double js_trunc(double x)
{
    if (!(js_fabs(x) < 4503599627370496.0) || x == 0)
        return x;
    double t = (double)(int64_t)x;
    if (t == 0 && x < 0)
        return -0.0;
    return t;
}

double js_floor(double x)
{
    if (!(js_fabs(x) < 4503599627370496.0) || x == 0)
        return x;
    double t = (double)(int64_t)x;
    if (t > x)
        t -= 1.0;
    return t;
}

double js_ceil(double x)
{
    if (!(js_fabs(x) < 4503599627370496.0) || x == 0)
        return x;
    double t = (double)(int64_t)x;
    if (t < x)
        t += 1.0;
    if (t == 0 && x < 0)
        return -0.0;
    return t;
}

double js_round(double x)
{
    if (!(js_fabs(x) < 4503599627370496.0) || x == 0)
        return x;
    double r = js_floor(x);
    if (x - r >= 0.5)
        r += 1.0;
    if (r == 0 && x < 0)
        return -0.0;
    return r;
}

double js_ldexp(double x, int e)
{
    if (x == 0 || !js_isfinite(x))
        return x;
    while (e > 1000) {
        x *= bitsd(0x7E70000000000000ULL);          /* 2^1000 */
        e -= 1000;
        if (js_isinf(x))
            return x;
    }
    while (e < -1000) {
        x *= bitsd(0x0170000000000000ULL);          /* 2^-1000 */
        e += 1000;
        if (x == 0)
            return x;
    }
    if (e > 0) {
        while (e > 0) {
            int s = e > 1000 ? 1000 : e;
            x *= bitsd((uint64_t)(1023 + s) << 52);
            e -= s;
        }
    } else if (e < 0) {
        while (e < 0) {
            int s = e < -1000 ? -1000 : e;
            x *= bitsd((uint64_t)(1023 + s) << 52);
            e -= s;
        }
    }
    return x;
}

/* Wykladnik potegi 2: x = m * 2^e, 1 <= |m| < 2 (x skonczony, niezerowy) */
static int ilogb(double x)
{
    int be = (int)((dbits(x) >> 52) & 0x7FF);
    if (be == 0) {
        x *= 18014398509481984.0;                     /* 2^54 */
        be = (int)((dbits(x) >> 52) & 0x7FF) - 54;
    }
    return be - 1023;
}

double js_fmod(double x, double y)
{
    if (js_isnan(x) || js_isnan(y) || js_isinf(x) || y == 0)
        return js_nan;
    if (js_isinf(y) || x == 0)
        return x;
    double ax = js_fabs(x), ay = js_fabs(y);
    if (ax < ay)
        return x;
    double r = ax;
    while (r >= ay) {
        int k = ilogb(r) - ilogb(ay);
        double t = js_ldexp(ay, k);
        if (t > r)
            t = js_ldexp(ay, k - 1);
        r -= t;                                      /* dokladne (lemat Sterbenza) */
    }
    return x < 0 ? -r : r;
}

/* Sprowadzenie x do [-pi/4, pi/4]: x = n * pi/2 + (y0 + y1) */
static int rem_pio2(double x, double *y0, double *y1)
{
    static const double invpio2 = 6.36619772367581382433e-01,
        pio2_1 = 1.57079632673412561417e+00, pio2_1t = 6.07710050650619224932e-11,
        pio2_2 = 6.07710050630396597660e-11, pio2_2t = 2.02226624879595063154e-21,
        pio2_3 = 2.02226624871116645580e-21, pio2_3t = 8.47842766036889956997e-32;
    if (js_fabs(x) <= 0.785398163397448279) {
        *y0 = x;
        *y1 = 0;
        return 0;
    }
    double fn = js_round(x * invpio2);
    int n = (int)(int64_t)fn;
    double r = x - fn * pio2_1;
    double w = fn * pio2_1t;
    double y = r - w;
    /* Kolejne kawalki pi/2 tylko gdy odejmowanie "zjadlo" duzo bitow */
    int j = (int)(dbits(x) >> 52 & 0x7FF);
    if (j - (int)(dbits(y) >> 52 & 0x7FF) > 16) {
        double t = r;
        w = fn * pio2_2;
        r = t - w;
        w = fn * pio2_2t - ((t - r) - w);
        y = r - w;
        if (j - (int)(dbits(y) >> 52 & 0x7FF) > 49) {
            t = r;
            w = fn * pio2_3;
            r = t - w;
            w = fn * pio2_3t - ((t - r) - w);
            y = r - w;
        }
    }
    *y0 = y;
    *y1 = (r - y) - w;
    return n;
}

static double k_sin(double x, double y, int iy)
{
    static const double S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
        S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
        S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10;
    double z = x * x, v = z * x;
    double r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    if (!iy)
        return x + v * (S1 + z * r);
    return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

static double k_cos(double x, double y)
{
    static const double C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
        C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
        C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;
    double z = x * x;
    double r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    double hz = 0.5 * z, w = 1.0 - hz;
    return w + (((1.0 - w) - hz) + (z * r - x * y));
}

double js_sin(double x)
{
    if (!js_isfinite(x))
        return js_nan;
    if (js_fabs(x) < 1e-8)
        return x;
    double y0, y1;
    int n = rem_pio2(x, &y0, &y1);
    switch (n & 3) {
    case 0: return k_sin(y0, y1, 1);
    case 1: return k_cos(y0, y1);
    case 2: return -k_sin(y0, y1, 1);
    default: return -k_cos(y0, y1);
    }
}

double js_cos(double x)
{
    if (!js_isfinite(x))
        return js_nan;
    double y0, y1;
    int n = rem_pio2(x, &y0, &y1);
    switch (n & 3) {
    case 0: return k_cos(y0, y1);
    case 1: return -k_sin(y0, y1, 1);
    case 2: return -k_cos(y0, y1);
    default: return k_sin(y0, y1, 1);
    }
}

/* Mlodsze 32 bity liczby wyzerowane (sztuczka fdlibm do dokladnego dzielenia) */
static double low_zero(double x)
{
    return bitsd(dbits(x) & 0xFFFFFFFF00000000ULL);
}

static double k_tan(double x, double y, int odd)
{
    static const double T[] = {
        3.33333333333334091986e-01, 1.33333333333201242699e-01, 5.39682539762260521377e-02,
        2.18694882948595424599e-02, 8.86323982359930005737e-03, 3.59207910759131235356e-03,
        1.45620945432529025516e-03, 5.88041240820264096874e-04, 2.46463134818469906812e-04,
        7.81794442939557092300e-05, 7.14072491382608190305e-05, -1.85586374855275456654e-05,
        2.59073051863633712884e-05,
    };
    static const double pio4 = 7.85398163397448278999e-01, pio4lo = 3.06161699786838301793e-17;
    int big = js_fabs(x) >= 0.6744, sign = 0;
    if (big) {
        sign = x < 0;
        if (sign) {
            x = -x;
            y = -y;
        }
        x = (pio4 - x) + (pio4lo - y);
        y = 0.0;
    }
    double z = x * x, w = z * z;
    double r = T[1] + w * (T[3] + w * (T[5] + w * (T[7] + w * (T[9] + w * T[11]))));
    double v = z * (T[2] + w * (T[4] + w * (T[6] + w * (T[8] + w * (T[10] + w * T[12])))));
    double s = z * x;
    r = y + z * (s * (r + v) + y) + s * T[0];
    w = x + r;
    if (big) {
        s = 1 - 2 * odd;
        v = s - 2.0 * (x + (r - w * w / (w + s)));
        return sign ? -v : v;
    }
    if (!odd)
        return w;
    double w0 = low_zero(w);
    v = r - (w0 - x);
    double a = -1.0 / w, a0 = low_zero(a);
    return a0 + a * (1.0 + a0 * w0 + a0 * v);
}

double js_tan(double x)
{
    if (!js_isfinite(x))
        return js_nan;
    if (js_fabs(x) < 1e-8)
        return x;
    double y0, y1;
    int n = rem_pio2(x, &y0, &y1);
    return k_tan(y0, y1, n & 1);
}

double js_atan(double x)
{
    static const double atanhi[] = { 4.63647609000806093515e-01, 7.85398163397448278999e-01,
                                     9.82793723247329054082e-01, 1.57079632679489655800e+00 };
    static const double atanlo[] = { 2.26987774529616870924e-17, 3.06161699786838301793e-17,
                                     1.39033110312309984516e-17, 6.12323399573676603587e-17 };
    static const double aT[] = {
        3.33333333333329318027e-01, -1.99999999998764832476e-01, 1.42857142725034663711e-01,
        -1.11111104054623557880e-01, 9.09088713343650656196e-02, -7.69187620504482999495e-02,
        6.66107313738753120669e-02, -5.83357013379057348645e-02, 4.97687799461593236017e-02,
        -3.65315727442169155270e-02, 1.62858201153657823623e-02,
    };
    if (js_isnan(x))
        return x;
    int neg = x < 0;
    double ax = js_fabs(x);
    if (ax >= 7.3786976294838206464e19)              /* 2^66 */
        return neg ? -atanhi[3] - atanlo[3] : atanhi[3] + atanlo[3];
    int id;
    if (ax < 0.4375) {
        if (ax < 1e-9)
            return x;
        id = -1;
    } else if (ax < 1.1875) {
        if (ax < 0.6875) {
            id = 0;
            ax = (2.0 * ax - 1.0) / (2.0 + ax);
        } else {
            id = 1;
            ax = (ax - 1.0) / (ax + 1.0);
        }
    } else if (ax < 2.4375) {
        id = 2;
        ax = (ax - 1.5) / (1.0 + 1.5 * ax);
    } else {
        id = 3;
        ax = -1.0 / ax;
    }
    double z = ax * ax, w = z * z;
    double s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    double s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    if (id < 0)
        return neg ? -(ax - ax * (s1 + s2)) : ax - ax * (s1 + s2);
    z = atanhi[id] - ((ax * (s1 + s2) - atanlo[id]) - ax);
    return neg ? -z : z;
}

double js_atan2(double y, double x)
{
    static const double pi = 3.1415926535897931160E+00, pi_lo = 1.2246467991473531772E-16;
    if (js_isnan(x) || js_isnan(y))
        return js_nan;
    if (x == 1.0)
        return js_atan(y);
    int ysign = dbits(y) >> 63, xsign = dbits(x) >> 63;
    if (y == 0) {
        if (!xsign)
            return y;
        return ysign ? -pi : pi;
    }
    if (x == 0)
        return ysign ? -1.5707963267948966 : 1.5707963267948966;
    if (js_isinf(x)) {
        if (js_isinf(y)) {
            double r = xsign ? 3 * 0.78539816339744830962 : 0.78539816339744830962;
            return ysign ? -r : r;
        }
        double r = xsign ? pi : 0.0;
        return ysign ? -r : r;
    }
    if (js_isinf(y))
        return ysign ? -1.5707963267948966 : 1.5707963267948966;
    double z;
    int k = ilogb(y) - ilogb(x);
    if (k > 60)
        z = 1.5707963267948966 + 0.5 * pi_lo;
    else if (xsign && k < -60)
        z = 0.0;
    else
        z = js_atan(js_fabs(y / x));
    if (!xsign)
        return ysign ? -z : z;
    z = pi - (z - pi_lo);
    return ysign ? -z : z;
}

double js_asin(double x)
{
    if (js_isnan(x) || x > 1 || x < -1)
        return js_nan;
    return js_atan2(x, js_sqrt((1 - x) * (1 + x)));
}

double js_acos(double x)
{
    if (js_isnan(x) || x > 1 || x < -1)
        return js_nan;
    return js_atan2(js_sqrt((1 - x) * (1 + x)), x);
}

double js_exp(double x)
{
    static const double ln2HI = 6.93147180369123816490e-01, ln2LO = 1.90821492927058770002e-10,
        invln2 = 1.44269504088896338700e+00, P1 = 1.66666666666666019037e-01,
        P2 = -2.77777777770155933842e-03, P3 = 6.61375632143793436117e-05,
        P4 = -1.65339022054652515390e-06, P5 = 4.13813679705723846039e-08;
    if (js_isnan(x))
        return x;
    if (x == 1)
        return 2.718281828459045;                    /* jak w V8: exp(1) to dokladnie Math.E */
    if (x > 709.782712893383973096)
        return js_inf;
    if (x < -745.13321910194110842)
        return 0;
    if (js_fabs(x) < 1e-12)
        return 1 + x;
    double k = js_round(x * invln2);
    double hi = x - k * ln2HI, lo = k * ln2LO;
    double r = hi - lo;
    double z = r * r;
    double c = r - z * (P1 + z * (P2 + z * (P3 + z * (P4 + z * P5))));
    double y = 1 - ((lo - (r * c) / (2 - c)) - hi);
    return js_ldexp(y, (int)k);
}

double js_log(double x)
{
    static const double ln2_hi = 6.93147180369123816490e-01, ln2_lo = 1.90821492927058770002e-10,
        Lg1 = 6.666666666666735130e-01, Lg2 = 3.999999999940941908e-01,
        Lg3 = 2.857142874366239149e-01, Lg4 = 2.222219843214978396e-01,
        Lg5 = 1.818357216161805012e-01, Lg6 = 1.531383769920937332e-01,
        Lg7 = 1.479819860511658591e-01;
    if (js_isnan(x) || x < 0)
        return js_nan;
    if (x == 0)
        return -js_inf;
    if (js_isinf(x))
        return x;
    int k = ilogb(x);
    double m = js_ldexp(x, -k);                       /* 1 <= m < 2 */
    if (m > 1.41421356237309504880) {
        m *= 0.5;
        k++;
    }
    double f = m - 1.0;
    double s = f / (2.0 + f), z = s * s, w = z * z;
    double t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    double t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    double R = t2 + t1, hfsq = 0.5 * f * f;
    return k * ln2_hi - ((hfsq - (s * (hfsq + R) + k * ln2_lo)) - f);
}

double js_expm1(double x)
{
    if (js_fabs(x) < 1e-5)
        return x + x * x / 2 + x * x * x / 6;
    return js_exp(x) - 1;
}

double js_log1p(double x)
{
    if (js_fabs(x) < 1e-4)
        return x - x * x / 2 + x * x * x / 3;
    double u = 1 + x;
    if (u == 1)
        return x;
    return js_log(u) - ((u - 1) - x) / u;
}

double js_pow(double x, double y)
{
    if (js_isnan(y))
        return js_nan;
    if (y == 0)
        return 1;
    if (js_isnan(x))
        return js_nan;
    double ax = js_fabs(x);
    if (js_isinf(y)) {
        if (ax == 1)
            return js_nan;
        if (ax > 1)
            return y > 0 ? js_inf : 0;
        return y > 0 ? 0 : js_inf;
    }
    int yint = y == js_trunc(y);
    int yodd = yint && js_fabs(y) < 9007199254740992.0 && ((int64_t)y & 1);
    if (x == 0) {
        int neg = dbits(x) >> 63;
        if (y > 0)
            return neg && yodd ? -0.0 : 0.0;
        return neg && yodd ? -js_inf : js_inf;
    }
    if (js_isinf(x)) {
        if (x > 0)
            return y > 0 ? js_inf : 0;
        if (y > 0)
            return yodd ? -js_inf : js_inf;
        return yodd ? -0.0 : 0.0;
    }
    if (x < 0 && !yint)
        return js_nan;
    if (y == 0.5)
        return js_sqrt(x);
    if (y == -0.5)
        return 1 / js_sqrt(x);
    if (yint && js_fabs(y) <= 1024) {
        int64_t n = (int64_t)js_fabs(y);
        double r = 1, b = x;
        while (n) {
            if (n & 1)
                r *= b;
            b *= b;
            n >>= 1;
        }
        return y < 0 ? 1 / r : r;
    }
    double r = js_exp(y * js_log(ax));
    return x < 0 && yodd ? -r : r;
}

double js_cbrt(double x)
{
    if (x == 0 || !js_isfinite(x))
        return x;
    double ax = js_fabs(x);
    double y = js_exp(js_log(ax) / 3);
    y = y - (y * y * y - ax) / (3 * y * y);
    y = y - (y * y * y - ax) / (3 * y * y);
    double r = js_round(y);
    if (r * r * r == ax)
        y = r;
    return x < 0 ? -y : y;
}

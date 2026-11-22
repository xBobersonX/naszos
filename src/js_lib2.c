/* Obiekty wbudowane JavaScriptu (czesc 2): String, Date, Map, Set,
 * Promise, iteracja (for-of, ...rozwiniecie), raporty bledow. */

#include "js_int.h"
#include "string.h"
#include "timer.h"
#include "rtc.h"

#define U __attribute__((unused))
#define NATIVE(name) static jv name(js_rt *rt U, jv self U, int argc U, jv *argv U, jobj *fn U)
#define ARG(i) ((i) < argc ? argv[i] : jv_undef())

void js_regex_string_init(js_rt *rt);

static jstr *S(js_rt *rt, const char *s) { return js_str_c(rt, s); }

/* ---------- String ---------- */

static jstr *this_str(js_rt *rt, jv self)
{
    if (self.t == JT_STR)
        return self.u.s;
    if (self.t == JT_OBJ && self.u.o->cls == JC_STR)
        return self.u.o->u.prim.u.s;
    if (IS_NULLISH(self))
        js_throw_error(rt, JS_ERR_TYPE, "String.prototype method called on null or undefined");
    return js_to_str(rt, self);
}

static uint32_t clamp_pos(js_rt *rt, jv v, uint32_t len, uint32_t def)
{
    if (v.t == JT_UNDEF)
        return def;
    double d = js_to_integer(rt, v);
    if (d < 0)
        return 0;
    if (d > len)
        return len;
    return (uint32_t)d;
}

static uint32_t rel_pos(js_rt *rt, jv v, uint32_t len, uint32_t def)
{
    if (v.t == JT_UNDEF)
        return def;
    double d = js_to_integer(rt, v);
    if (d < 0) {
        d += len;
        if (d < 0)
            d = 0;
    }
    if (d > len)
        d = len;
    return (uint32_t)d;
}

static jv string_call(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn U)
{
    if (!argc)
        return jv_str(rt->empty);
    if (argv[0].t == JT_STR)
        return argv[0];
    return jv_str(js_to_str(rt, argv[0]));
}

static jv string_ctor(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn U)
{
    return jv_obj(js_to_obj(rt, jv_str(argc ? js_to_str(rt, argv[0]) : rt->empty)));
}

NATIVE(str_from_char_code)
{
    jstr *s = js_str_new(rt, (uint32_t)argc);
    for (int i = 0; i < argc; i++)
        s->c[i] = (uint16_t)js_to_uint32(rt, argv[i]);
    return jv_str(s);
}

NATIVE(str_from_code_point)
{
    struct jsb b;
    jsb_init(&b, rt);
    for (int i = 0; i < argc; i++) {
        double d = js_to_num(rt, argv[i]);
        if (d < 0 || d > 0x10FFFF || d != js_trunc(d))
            js_throw_error(rt, JS_ERR_RANGE, "Invalid code point");
        uint32_t cp = (uint32_t)d;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            jsb_char(&b, (uint16_t)(0xD800 + (cp >> 10)));
            jsb_char(&b, (uint16_t)(0xDC00 + (cp & 0x3FF)));
        } else {
            jsb_char(&b, (uint16_t)cp);
        }
    }
    return jv_str(jsb_done(&b));
}

NATIVE(str_raw)
{
    jv strings = ARG(0);
    jv raw = js_get(rt, strings, rt->A.raw);
    double n = js_to_integer(rt, js_get(rt, raw, rt->A.length));
    struct jsb b;
    jsb_init(&b, rt);
    for (uint32_t i = 0; i < (uint32_t)n; i++) {
        jsb_str(&b, js_to_str(rt, js_get_index(rt, raw, jv_num(i))));
        if (i + 1 < (uint32_t)n && (int)i + 1 < argc)
            jsb_str(&b, js_to_str(rt, argv[i + 1]));
    }
    return jv_str(jsb_done(&b));
}

NATIVE(strp_to_string)
{
    if (self.t == JT_STR)
        return self;
    if (self.t == JT_OBJ && self.u.o->cls == JC_STR)
        return self.u.o->u.prim;
    js_throw_error(rt, JS_ERR_TYPE, "String.prototype.toString requires that 'this' be a String");
}

NATIVE(strp_char_at)
{
    jstr *s = this_str(rt, self);
    double i = js_to_integer(rt, ARG(0));
    if (i < 0 || i >= s->len)
        return jv_str(rt->empty);
    return jv_str(js_substr(rt, s, (uint32_t)i, (uint32_t)i + 1));
}

NATIVE(strp_char_code_at)
{
    jstr *s = this_str(rt, self);
    double i = js_to_integer(rt, ARG(0));
    if (i < 0 || i >= s->len)
        return jv_num(js_nan);
    return jv_num(s->c[(uint32_t)i]);
}

NATIVE(strp_code_point_at)
{
    jstr *s = this_str(rt, self);
    double i = js_to_integer(rt, ARG(0));
    if (i < 0 || i >= s->len)
        return jv_undef();
    uint32_t k = (uint32_t)i;
    uint32_t c = s->c[k];
    if (c >= 0xD800 && c < 0xDC00 && k + 1 < s->len && s->c[k + 1] >= 0xDC00 && s->c[k + 1] < 0xE000)
        c = 0x10000 + ((c - 0xD800) << 10) + (s->c[k + 1] - 0xDC00);
    return jv_num(c);
}

NATIVE(strp_at)
{
    jstr *s = this_str(rt, self);
    double i = js_to_integer(rt, ARG(0));
    if (i < 0)
        i += s->len;
    if (i < 0 || i >= s->len)
        return jv_undef();
    return jv_str(js_substr(rt, s, (uint32_t)i, (uint32_t)i + 1));
}

static int64_t str_find(const jstr *s, const jstr *w, uint32_t from)
{
    if (w->len == 0)
        return from <= s->len ? from : s->len;
    if (w->len > s->len)
        return -1;
    for (uint32_t i = from; i + w->len <= s->len; i++)
        if (s->c[i] == w->c[0] && memcmp(s->c + i, w->c, (size_t)w->len * 2) == 0)
            return i;
    return -1;
}

NATIVE(strp_index_of)
{
    jstr *s = this_str(rt, self), *w = js_to_str(rt, ARG(0));
    return jv_num((double)str_find(s, w, clamp_pos(rt, ARG(1), s->len, 0)));
}

NATIVE(strp_last_index_of)
{
    jstr *s = this_str(rt, self), *w = js_to_str(rt, ARG(0));
    double pos = ARG(1).t == JT_UNDEF ? js_nan : js_to_num(rt, argv[1]);
    int64_t start = js_isnan(pos) ? (int64_t)s->len : (int64_t)js_trunc(pos);
    if (start > (int64_t)s->len - (int64_t)w->len)
        start = (int64_t)s->len - (int64_t)w->len;
    for (int64_t i = start; i >= 0; i--)
        if (memcmp(s->c + i, w->c, (size_t)w->len * 2) == 0)
            return jv_num((double)i);
    return jv_num(-1);
}

static void no_regexp(js_rt *rt, jv v, const char *name)
{
    if (v.t == JT_OBJ && v.u.o->cls == JC_REGEXP)
        js_throw_error(rt, JS_ERR_TYPE, "First argument to String.prototype.%s must not be a regular expression", name);
}

NATIVE(strp_includes)
{
    no_regexp(rt, ARG(0), "includes");
    jstr *s = this_str(rt, self), *w = js_to_str(rt, ARG(0));
    return jv_bool(str_find(s, w, clamp_pos(rt, ARG(1), s->len, 0)) >= 0);
}

NATIVE(strp_starts_with)
{
    no_regexp(rt, ARG(0), "startsWith");
    jstr *s = this_str(rt, self), *w = js_to_str(rt, ARG(0));
    uint32_t p = clamp_pos(rt, ARG(1), s->len, 0);
    return jv_bool(p + w->len <= s->len && memcmp(s->c + p, w->c, (size_t)w->len * 2) == 0);
}

NATIVE(strp_ends_with)
{
    no_regexp(rt, ARG(0), "endsWith");
    jstr *s = this_str(rt, self), *w = js_to_str(rt, ARG(0));
    uint32_t e = clamp_pos(rt, ARG(1), s->len, s->len);
    return jv_bool(w->len <= e && memcmp(s->c + e - w->len, w->c, (size_t)w->len * 2) == 0);
}

NATIVE(strp_slice)
{
    jstr *s = this_str(rt, self);
    uint32_t a = rel_pos(rt, ARG(0), s->len, 0), b = rel_pos(rt, ARG(1), s->len, s->len);
    return jv_str(js_substr(rt, s, a, b));
}

NATIVE(strp_substring)
{
    jstr *s = this_str(rt, self);
    uint32_t a = clamp_pos(rt, ARG(0), s->len, 0), b = clamp_pos(rt, ARG(1), s->len, s->len);
    if (a > b) {
        uint32_t t = a;
        a = b;
        b = t;
    }
    return jv_str(js_substr(rt, s, a, b));
}

NATIVE(strp_substr)
{
    jstr *s = this_str(rt, self);
    uint32_t a = rel_pos(rt, ARG(0), s->len, 0);
    double n = ARG(1).t == JT_UNDEF ? s->len : js_to_integer(rt, argv[1]);
    if (n <= 0)
        return jv_str(rt->empty);
    uint32_t b = a + n > s->len ? s->len : a + (uint32_t)n;
    return jv_str(js_substr(rt, s, a, b));
}

NATIVE(strp_upper)
{
    jstr *s = this_str(rt, self);
    struct jsb b;
    jsb_init(&b, rt);
    for (uint32_t i = 0; i < s->len; i++) {
        if (s->c[i] == 0xDF) {                      /* ss */
            jsb_ascii(&b, "SS");
            continue;
        }
        jsb_char(&b, js_upper(s->c[i]));
    }
    return jv_str(jsb_done(&b));
}

NATIVE(strp_lower)
{
    jstr *s = this_str(rt, self);
    jstr *r = js_str_new(rt, s->len);
    for (uint32_t i = 0; i < s->len; i++)
        r->c[i] = js_lower(s->c[i]);
    return jv_str(r);
}

static jv trim(js_rt *rt, jv self, int left, int right)
{
    jstr *s = this_str(rt, self);
    uint32_t a = 0, b = s->len;
    while (left && a < b && js_is_space(s->c[a]))
        a++;
    while (right && b > a && js_is_space(s->c[b - 1]))
        b--;
    return jv_str(js_substr(rt, s, a, b));
}

NATIVE(strp_trim) { return trim(rt, self, 1, 1); }
NATIVE(strp_trim_start) { return trim(rt, self, 1, 0); }
NATIVE(strp_trim_end) { return trim(rt, self, 0, 1); }

static jv pad(js_rt *rt, jv self, int argc, jv *argv, int start)
{
    jstr *s = this_str(rt, self);
    double target = js_to_integer(rt, ARG(0));
    jstr *fill = ARG(1).t == JT_UNDEF ? js_atom(rt, " ") : js_to_str(rt, argv[1]);
    if (target <= s->len || !fill->len)
        return jv_str(s);
    if (target > (1 << 26))
        js_throw_error(rt, JS_ERR_RANGE, "Invalid string length");
    uint32_t need = (uint32_t)target - s->len;
    struct jsb b;
    jsb_init(&b, rt);
    if (!start)
        jsb_str(&b, s);
    for (uint32_t i = 0; i < need; i++)
        jsb_char(&b, fill->c[i % fill->len]);
    if (start)
        jsb_str(&b, s);
    return jv_str(jsb_done(&b));
}

NATIVE(strp_pad_start) { return pad(rt, self, argc, argv, 1); }
NATIVE(strp_pad_end) { return pad(rt, self, argc, argv, 0); }

NATIVE(strp_repeat)
{
    jstr *s = this_str(rt, self);
    double n = js_to_integer(rt, ARG(0));
    if (n < 0 || js_isinf(n))
        js_throw_error(rt, JS_ERR_RANGE, "Invalid count value: %d", (int)n);
    if (n * s->len > (1 << 27))
        js_throw_error(rt, JS_ERR_RANGE, "Invalid string length");
    struct jsb b;
    jsb_init(&b, rt);
    for (uint32_t i = 0; i < (uint32_t)n; i++)
        jsb_str(&b, s);
    return jv_str(jsb_done(&b));
}

NATIVE(strp_concat)
{
    jstr *s = this_str(rt, self);
    for (int i = 0; i < argc; i++)
        s = js_concat(rt, s, js_to_str(rt, argv[i]));
    return jv_str(s);
}

/* Porownanie "jak w slowniku": najpierw bez wielkosci liter i ogonkow */
static uint16_t base_letter(uint16_t c)
{
    static const struct { uint16_t c, b; } map[] = {
        { 0x105, 'a' }, { 0x107, 'c' }, { 0x119, 'e' }, { 0x142, 'l' }, { 0x144, 'n' },
        { 0xF3, 'o' }, { 0x15B, 's' }, { 0x17A, 'z' }, { 0x17C, 'z' }, { 0xE9, 'e' }, { 0xE1, 'a' },
        { 0xE4, 'a' }, { 0xF6, 'o' }, { 0xFC, 'u' }, { 0xE0, 'a' }, { 0xE8, 'e' }, { 0xED, 'i' },
    };
    c = js_lower(c);
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (map[i].c == c)
            return map[i].b;
    return c;
}

static int polish_rank(uint16_t c)
{
    /* kolejnosc polskiego alfabetu: a a(ogonek) b c c(kreska) ... */
    uint16_t l = js_lower(c), b = base_letter(c);
    if (b >= 'a' && b <= 'z')
        return b * 4 + (l != b ? (l == 0x17C ? 2 : 1) : 0);
    return l * 4 + 1000;
}

NATIVE(strp_locale_compare)
{
    jstr *a = this_str(rt, self), *b = js_to_str(rt, ARG(0));
    uint32_t n = a->len < b->len ? a->len : b->len;
    for (uint32_t i = 0; i < n; i++) {
        int ra = polish_rank(a->c[i]), rb = polish_rank(b->c[i]);
        if (ra != rb)
            return jv_num(ra < rb ? -1 : 1);
    }
    if (a->len != b->len)
        return jv_num(a->len < b->len ? -1 : 1);
    for (uint32_t i = 0; i < n; i++)                /* male litery przed wielkimi */
        if (a->c[i] != b->c[i])
            return jv_num(a->c[i] > b->c[i] ? -1 : 1);
    return jv_num(0);
}

NATIVE(strp_normalize) { return jv_str(this_str(rt, self)); }

NATIVE(strp_iterator) { return jv_obj(js_make_iter(rt, js_iterate_to_array(rt, jv_str(this_str(rt, self))).u.o, IT_VALUES)); }

/* ---------- Date ---------- */

static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int64_t *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yy + (*m <= 2);
}

static double now_ms(js_rt *rt)
{
    return (double)rt->clock_base_ms + (double)(uint32_t)(timer_uptime_ms() - rt->clock_base_uptime);
}

jv js_date_now_ms(void)
{
    return jv_undef();
}

/* Przesuniecie czasu polskiego (ms) dla chwili UTC t */
static double tz_offset(double t)
{
    if (!js_isfinite(t))
        return 0;
    return rtc_poland_offset((int64_t)js_floor(t / 1000)) * 1000.0;
}

/* Czas lokalny (ms) -> UTC */
static double local_to_utc(double lt)
{
    double off = tz_offset(lt - 3600000.0);
    return lt - off;
}

struct dparts {
    int64_t year;
    int month, day, hour, min, sec, ms, wday;
};

static void split_time(double t, int local, struct dparts *p)
{
    double lt = local ? t + tz_offset(t) : t;
    double days = js_floor(lt / 86400000.0);
    double msday = lt - days * 86400000.0;
    civil_from_days((int64_t)days, &p->year, &p->month, &p->day);
    p->month -= 1;
    int64_t ms = (int64_t)msday;
    p->hour = (int)(ms / 3600000);
    p->min = (int)(ms / 60000 % 60);
    p->sec = (int)(ms / 1000 % 60);
    p->ms = (int)(ms % 1000);
    int64_t w = ((int64_t)days + 4) % 7;
    p->wday = (int)(w < 0 ? w + 7 : w);
}

/* rok, miesiac (0-11, moze wyjsc poza zakres), dzien, godziny... -> ms */
static double make_time(double y, double mo, double d, double h, double mi, double s, double ms)
{
    if (!js_isfinite(y) || !js_isfinite(mo) || !js_isfinite(d) || !js_isfinite(h) || !js_isfinite(mi) ||
        !js_isfinite(s) || !js_isfinite(ms))
        return js_nan;
    y = js_trunc(y);
    mo = js_trunc(mo);
    double ym = y + js_floor(mo / 12);
    double mn = mo - js_floor(mo / 12) * 12;
    if (js_fabs(ym) > 400000)
        return js_nan;
    double days = (double)days_from_civil((int64_t)ym, (int)mn + 1, 1) + js_trunc(d) - 1;
    return days * 86400000.0 + js_trunc(h) * 3600000.0 + js_trunc(mi) * 60000.0 + js_trunc(s) * 1000.0 + js_trunc(ms);
}

static double time_clip(double t)
{
    if (!js_isfinite(t) || js_fabs(t) > 8.64e15)
        return js_nan;
    return js_trunc(t) + 0.0;
}

static const char *const mon_en[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *const mon_en_long[12] = { "January", "February", "March", "April", "May", "June", "July",
                                             "August", "September", "October", "November", "December" };
static const char *const day_en[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const day_en_long[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const mon_pl_gen[12] = {
    "stycznia", "lutego", "marca", "kwietnia", "maja", "czerwca", "lipca", "sierpnia",
    "wrze\xC5\x9Bnia", ("pa\xC5\xBA" "dziernika"), "listopada", "grudnia",
};
static const char *const mon_pl[12] = {
    "stycze\xC5\x84", "luty", "marzec", "kwiecie\xC5\x84", "maj", "czerwiec", "lipiec",
    "sierpie\xC5\x84", "wrzesie\xC5\x84", ("pa\xC5\xBA" "dziernik"), "listopad", "grudzie\xC5\x84",
};
static const char *const mon_pl_short[12] = { "sty", "lut", "mar", "kwi", "maj", "cze", "lip", "sie", "wrz", "pa\xC5\xBA", "lis", "gru" };
static const char *const day_pl[7] = {
    "niedziela", ("poniedzia\xC5\x82" "ek"), "wtorek", "\xC5\x9Broda", "czwartek", "pi\xC4\x85tek", "sobota",
};
static const char *const day_pl_short[7] = { "niedz.", "pon.", "wt.", "\xC5\x9Br.", "czw.", "pt.", "sob." };

static int parse_month(const uint16_t *c, uint32_t n)
{
    if (n < 3)
        return -1;
    for (int i = 0; i < 12; i++)
        if (js_lower(c[0]) == (uint16_t)(mon_en[i][0] | 0x20) && js_lower(c[1]) == (uint16_t)mon_en[i][1] &&
            js_lower(c[2]) == (uint16_t)mon_en[i][2])
            return i;
    return -1;
}

/* Date.parse: ISO 8601 i popularne zapisy ("Sep 30, 2026 12:00", "2026/09/30") */
static double parse_date(js_rt *rt, jstr *s)
{
    (void)rt;
    uint32_t i = 0, n = s->len;
    const uint16_t *c = s->c;
    while (i < n && js_is_space(c[i]))
        i++;
    /* ISO: YYYY-MM-DD[THH:MM[:SS[.mmm]]][Z|+HH:MM] */
    if (n - i >= 4 && c[i] >= '0' && c[i] <= '9' && c[i + 1] >= '0' && c[i + 1] <= '9' && c[i + 2] >= '0' &&
        c[i + 3] >= '0' && (n - i == 4 || c[i + 4] == '-')) {
        int v[7] = { 0, 1, 1, 0, 0, 0, 0 };
        uint32_t k = i;
        int field = 0, has_time = 0, zone = 0, tzsign = 0, tzh = 0, tzm = 0;
        while (k < n && field < 7) {
            int num = 0, digits = 0;
            while (k < n && c[k] >= '0' && c[k] <= '9') {
                if (digits < 9)
                    num = num * 10 + (c[k] - '0');
                digits++;
                k++;
            }
            if (!digits)
                break;
            if (field == 6) {                          /* ulamek sekundy */
                while (digits > 3) {
                    num /= 10;
                    digits--;
                }
                while (digits < 3) {
                    num *= 10;
                    digits++;
                }
            }
            v[field++] = num;
            if (k >= n)
                break;
            uint16_t sep = c[k];
            if (field < 3 && sep == '-') { k++; continue; }
            if (field == 3 && (sep == 'T' || sep == ' ')) { k++; has_time = 1; continue; }
            if ((field == 4 || field == 5) && sep == ':') { k++; continue; }
            if (field == 6 && sep == '.') { k++; continue; }
            break;
        }
        if (field >= 4)
            has_time = 1;
        if (k < n && (c[k] == 'Z' || c[k] == 'z')) {
            zone = 1;
            k++;
        } else if (k < n && (c[k] == '+' || c[k] == '-')) {
            tzsign = c[k] == '-' ? -1 : 1;
            k++;
            int d = 0;
            while (k < n && c[k] >= '0' && c[k] <= '9' && d < 2) { tzh = tzh * 10 + (c[k++] - '0'); d++; }
            if (k < n && c[k] == ':')
                k++;
            d = 0;
            while (k < n && c[k] >= '0' && c[k] <= '9' && d < 2) { tzm = tzm * 10 + (c[k++] - '0'); d++; }
            zone = 2;
        }
        while (k < n && js_is_space(c[k]))
            k++;
        if (k == n && field >= 1) {
            double t = make_time(v[0], v[1] - 1, v[2], v[3], v[4], v[5], v[6]);
            if (zone == 2)
                t -= tzsign * (tzh * 3600000.0 + tzm * 60000.0);
            else if (!zone && has_time)
                t = local_to_utc(t);
            return time_clip(t);
        }
    }
    /* Ogolnie: slowa i liczby */
    int nums[6], nn = 0, month = -1, hour = -1, min = 0, sec = 0, pm = -1, utc = 0, tzoff = 0, has_tz = 0;
    while (i < n) {
        uint16_t ch = c[i];
        if (ch >= '0' && ch <= '9') {
            int v = 0;
            uint32_t st = i;
            while (i < n && c[i] >= '0' && c[i] <= '9')
                v = v * 10 + (c[i++] - '0');
            if (i < n && c[i] == ':') {                /* godzina */
                hour = v;
                i++;
                min = 0;
                while (i < n && c[i] >= '0' && c[i] <= '9')
                    min = min * 10 + (c[i++] - '0');
                if (i < n && c[i] == ':') {
                    i++;
                    sec = 0;
                    while (i < n && c[i] >= '0' && c[i] <= '9')
                        sec = sec * 10 + (c[i++] - '0');
                }
                continue;
            }
            if (st > 0 && (c[st - 1] == '+' || c[st - 1] == '-') && (utc || hour >= 0)) {
                int sign = c[st - 1] == '-' ? -1 : 1;
                tzoff = sign * ((v / 100) * 60 + v % 100);
                has_tz = 1;
                continue;
            }
            if (nn < 6)
                nums[nn++] = v;
            continue;
        }
        if ((ch | 0x20) >= 'a' && (ch | 0x20) <= 'z') {
            uint32_t st = i;
            while (i < n && (c[i] | 0x20) >= 'a' && (c[i] | 0x20) <= 'z')
                i++;
            uint32_t len = i - st;
            int m = parse_month(c + st, len);
            if (m >= 0)
                month = m;
            else if (len == 2 && (c[st] | 0x20) == 'p' && (c[st + 1] | 0x20) == 'm')
                pm = 1;
            else if (len == 2 && (c[st] | 0x20) == 'a' && (c[st + 1] | 0x20) == 'm')
                pm = 0;
            else if ((len == 3 && (c[st] | 0x20) == 'g' && (c[st + 1] | 0x20) == 'm' && (c[st + 2] | 0x20) == 't') ||
                     (len == 3 && (c[st] | 0x20) == 'u' && (c[st + 1] | 0x20) == 't' && (c[st + 2] | 0x20) == 'c') ||
                     (len == 1 && (c[st] | 0x20) == 'z'))
                utc = 1;
            continue;
        }
        i++;
    }
    int64_t year = -1;
    int day = 1;
    if (month >= 0) {
        if (nn >= 2) {
            if (nums[0] > 31) { year = nums[0]; day = nums[1]; }
            else { day = nums[0]; year = nums[1]; }
        } else if (nn == 1) {
            year = nums[0];
        }
    } else if (nn >= 3) {
        if (nums[0] > 31) { year = nums[0]; month = nums[1] - 1; day = nums[2]; }
        else { month = nums[0] - 1; day = nums[1]; year = nums[2]; }
    }
    if (year < 0 || month < 0 || month > 11)
        return js_nan;
    if (year < 50)
        year += 2000;
    else if (year < 100)
        year += 1900;
    if (hour < 0)
        hour = 0;
    if (pm == 1 && hour < 12)
        hour += 12;
    if (pm == 0 && hour == 12)
        hour = 0;
    double t = make_time((double)year, month, day, hour, min, sec, 0);
    if (has_tz)
        t -= tzoff * 60000.0;
    else if (!utc)
        t = local_to_utc(t);
    return time_clip(t);
}

static double this_time(js_rt *rt, jv self)
{
    if (self.t != JT_OBJ || self.u.o->cls != JC_DATE)
        js_throw_error(rt, JS_ERR_TYPE, "this is not a Date object.");
    return self.u.o->u.prim.u.n;
}

static jv date_call(js_rt *rt, jv self U, int argc U, jv *argv U, jobj *fn U)
{
    jobj *d = js_new_object(rt, rt->date_proto);
    d->cls = JC_DATE;
    d->u.prim = jv_num(js_trunc(now_ms(rt)));
    jv f = js_get(rt, jv_obj(d), rt->A.toString);
    return js_call(rt, f, jv_obj(d), 0, 0);
}

static jv date_ctor(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn U)
{
    double t;
    if (argc == 0) {
        t = js_trunc(now_ms(rt));
    } else if (argc == 1) {
        jv v = argv[0];
        if (v.t == JT_OBJ && v.u.o->cls == JC_DATE)
            t = v.u.o->u.prim.u.n;
        else {
            v = js_to_prim(rt, v, 0);
            t = v.t == JT_STR ? parse_date(rt, v.u.s) : time_clip(js_to_num(rt, v));
        }
    } else {
        double f[7] = { 0, 0, 1, 0, 0, 0, 0 };
        for (int i = 0; i < 7 && i < argc; i++)
            f[i] = js_to_num(rt, argv[i]);
        if (js_isfinite(f[0]) && f[0] >= 0 && f[0] <= 99)
            f[0] += 1900;
        t = time_clip(local_to_utc(make_time(f[0], f[1], f[2], f[3], f[4], f[5], f[6])));
    }
    jobj *d = js_new_object(rt, rt->date_proto);
    d->cls = JC_DATE;
    d->u.prim = jv_num(t);
    return jv_obj(d);
}

NATIVE(date_now) { return jv_num(js_trunc(now_ms(rt))); }
NATIVE(date_parse) { return jv_num(parse_date(rt, js_to_str(rt, ARG(0)))); }

NATIVE(date_utc)
{
    double f[7] = { 0, 0, 1, 0, 0, 0, 0 };
    for (int i = 0; i < 7 && i < argc; i++)
        f[i] = js_to_num(rt, argv[i]);
    if (js_isfinite(f[0]) && f[0] >= 0 && f[0] <= 99)
        f[0] += 1900;
    return jv_num(time_clip(make_time(f[0], f[1], f[2], f[3], f[4], f[5], f[6])));
}

enum { DF_YEAR, DF_MONTH, DF_DATE, DF_DAY, DF_HOURS, DF_MINUTES, DF_SECONDS, DF_MS };

static jv date_get(js_rt *rt, jv self, int field, int local)
{
    double t = this_time(rt, self);
    if (js_isnan(t))
        return jv_num(js_nan);
    struct dparts p;
    split_time(t, local, &p);
    switch (field) {
    case DF_YEAR: return jv_num((double)p.year);
    case DF_MONTH: return jv_num(p.month);
    case DF_DATE: return jv_num(p.day);
    case DF_DAY: return jv_num(p.wday);
    case DF_HOURS: return jv_num(p.hour);
    case DF_MINUTES: return jv_num(p.min);
    case DF_SECONDS: return jv_num(p.sec);
    default: return jv_num(p.ms);
    }
}

#define DATE_GET(name, field, local) NATIVE(name) { return date_get(rt, self, field, local); }
DATE_GET(dp_get_full_year, DF_YEAR, 1)
DATE_GET(dp_get_month, DF_MONTH, 1)
DATE_GET(dp_get_date, DF_DATE, 1)
DATE_GET(dp_get_day, DF_DAY, 1)
DATE_GET(dp_get_hours, DF_HOURS, 1)
DATE_GET(dp_get_minutes, DF_MINUTES, 1)
DATE_GET(dp_get_seconds, DF_SECONDS, 1)
DATE_GET(dp_get_ms, DF_MS, 1)
DATE_GET(dp_get_utc_full_year, DF_YEAR, 0)
DATE_GET(dp_get_utc_month, DF_MONTH, 0)
DATE_GET(dp_get_utc_date, DF_DATE, 0)
DATE_GET(dp_get_utc_day, DF_DAY, 0)
DATE_GET(dp_get_utc_hours, DF_HOURS, 0)
DATE_GET(dp_get_utc_minutes, DF_MINUTES, 0)
DATE_GET(dp_get_utc_seconds, DF_SECONDS, 0)
DATE_GET(dp_get_utc_ms, DF_MS, 0)

NATIVE(dp_get_year)
{
    jv y = date_get(rt, self, DF_YEAR, 1);
    return jv_num(y.u.n - 1900);
}

NATIVE(dp_get_time) { return jv_num(this_time(rt, self)); }

NATIVE(dp_get_timezone_offset)
{
    double t = this_time(rt, self);
    if (js_isnan(t))
        return jv_num(js_nan);
    return jv_num(-tz_offset(t) / 60000.0);
}

NATIVE(dp_set_time)
{
    this_time(rt, self);
    double t = time_clip(js_to_num(rt, ARG(0)));
    self.u.o->u.prim = jv_num(t);
    return jv_num(t);
}

/* setHours(h, m, s, ms), setDate(d)... first = pierwsze pole zmieniane przez funkcje */
static jv date_set(js_rt *rt, jv self, int argc, jv *argv, int first, int local)
{
    double t = this_time(rt, self);
    struct dparts p;
    if (js_isnan(t)) {
        if (first != DF_YEAR)
            return jv_num(js_nan);
        t = 0;
    }
    split_time(t, local, &p);
    double f[7] = { (double)p.year, p.month, p.day, p.hour, p.min, p.sec, p.ms };
    static const int slot[] = { 0, 1, 2, -1, 3, 4, 5, 6 };
    int s = slot[first];
    int max_args = first == DF_YEAR ? 3 : first == DF_MONTH ? 2 : first == DF_DATE ? 1 :
                   first == DF_HOURS ? 4 : first == DF_MINUTES ? 3 : first == DF_SECONDS ? 2 : 1;
    for (int i = 0; i < argc && i < max_args; i++)
        f[s + i] = js_to_num(rt, argv[i]);
    double nt = make_time(f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
    if (local)
        nt = local_to_utc(nt);
    nt = time_clip(nt);
    self.u.o->u.prim = jv_num(nt);
    return jv_num(nt);
}

#define DATE_SET(name, field, local) NATIVE(name) { return date_set(rt, self, argc, argv, field, local); }
DATE_SET(dp_set_full_year, DF_YEAR, 1)
DATE_SET(dp_set_month, DF_MONTH, 1)
DATE_SET(dp_set_date, DF_DATE, 1)
DATE_SET(dp_set_hours, DF_HOURS, 1)
DATE_SET(dp_set_minutes, DF_MINUTES, 1)
DATE_SET(dp_set_seconds, DF_SECONDS, 1)
DATE_SET(dp_set_ms, DF_MS, 1)
DATE_SET(dp_set_utc_full_year, DF_YEAR, 0)
DATE_SET(dp_set_utc_month, DF_MONTH, 0)
DATE_SET(dp_set_utc_date, DF_DATE, 0)
DATE_SET(dp_set_utc_hours, DF_HOURS, 0)
DATE_SET(dp_set_utc_minutes, DF_MINUTES, 0)
DATE_SET(dp_set_utc_seconds, DF_SECONDS, 0)

static jv date_invalid(js_rt *rt) { return jv_str(S(rt, "Invalid Date")); }

NATIVE(dp_to_iso_string)
{
    double t = this_time(rt, self);
    if (js_isnan(t))
        js_throw_error(rt, JS_ERR_RANGE, "Invalid time value");
    struct dparts p;
    split_time(t, 0, &p);
    char buf[48];
    if (p.year >= 0 && p.year <= 9999)
        ksnprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", (int)p.year, p.month + 1, p.day,
                  p.hour, p.min, p.sec, p.ms);
    else
        ksnprintf(buf, sizeof(buf), "%c%06d-%02d-%02dT%02d:%02d:%02d.%03dZ", p.year < 0 ? '-' : '+',
                  (int)(p.year < 0 ? -p.year : p.year), p.month + 1, p.day, p.hour, p.min, p.sec, p.ms);
    return jv_str(S(rt, buf));
}

NATIVE(dp_to_json)
{
    double t = this_time(rt, self);
    if (js_isnan(t))
        return jv_null();
    return dp_to_iso_string(rt, self, argc, argv, fn);
}

static void tz_text(double t, char *buf, unsigned size)
{
    int off = (int)(tz_offset(t) / 60000);
    ksnprintf(buf, size, "GMT%c%02d%02d (%s)", off < 0 ? '-' : '+', (off < 0 ? -off : off) / 60, (off < 0 ? -off : off) % 60,
              off == 120 ? "Central European Summer Time" : "Central European Standard Time");
}

NATIVE(dp_to_string)
{
    double t = this_time(rt, self);
    if (js_isnan(t))
        return date_invalid(rt);
    struct dparts p;
    split_time(t, 1, &p);
    char buf[120], tz[64];
    tz_text(t, tz, sizeof(tz));
    ksnprintf(buf, sizeof(buf), "%s %s %02d %04d %02d:%02d:%02d %s", day_en[p.wday], mon_en[p.month], p.day,
              (int)p.year, p.hour, p.min, p.sec, tz);
    return jv_str(S(rt, buf));
}

NATIVE(dp_to_date_string)
{
    double t = this_time(rt, self);
    if (js_isnan(t))
        return date_invalid(rt);
    struct dparts p;
    split_time(t, 1, &p);
    char buf[64];
    ksnprintf(buf, sizeof(buf), "%s %s %02d %04d", day_en[p.wday], mon_en[p.month], p.day, (int)p.year);
    return jv_str(S(rt, buf));
}

NATIVE(dp_to_time_string)
{
    double t = this_time(rt, self);
    if (js_isnan(t))
        return date_invalid(rt);
    struct dparts p;
    split_time(t, 1, &p);
    char buf[100], tz[64];
    tz_text(t, tz, sizeof(tz));
    ksnprintf(buf, sizeof(buf), "%02d:%02d:%02d %s", p.hour, p.min, p.sec, tz);
    return jv_str(S(rt, buf));
}

NATIVE(dp_to_utc_string)
{
    double t = this_time(rt, self);
    if (js_isnan(t))
        return date_invalid(rt);
    struct dparts p;
    split_time(t, 0, &p);
    char buf[64];
    ksnprintf(buf, sizeof(buf), "%s, %02d %s %04d %02d:%02d:%02d GMT", day_en[p.wday], p.day, mon_en[p.month],
              (int)p.year, p.hour, p.min, p.sec);
    return jv_str(S(rt, buf));
}

/* toLocale*String: polski ("30.09.2026, 14:03:05") albo amerykanski zapis; opcje jak
 * { weekday: 'long', day: 'numeric', month: 'long', year: 'numeric' } */
static jstr *opt(js_rt *rt, jv o, const char *name)
{
    if (o.t != JT_OBJ)
        return 0;
    jv v = js_get(rt, o, js_atom(rt, name));
    return v.t == JT_STR ? v.u.s : 0;
}

static jv date_locale(js_rt *rt, jv self, int argc, jv *argv, int want_date, int want_time)
{
    double t = this_time(rt, self);
    if (js_isnan(t))
        return date_invalid(rt);
    int pl = ARG(0).t == JT_STR && argv[0].u.s->len >= 2 && argv[0].u.s->c[0] == 'p' && argv[0].u.s->c[1] == 'l';
    jv o = ARG(1);
    jstr *wd = opt(rt, o, "weekday"), *dd = opt(rt, o, "day"), *mm = opt(rt, o, "month"), *yy = opt(rt, o, "year"),
         *hh = opt(rt, o, "hour"), *mi = opt(rt, o, "minute"), *ss = opt(rt, o, "second");
    int any = wd || dd || mm || yy || hh || mi || ss;
    if (o.t == JT_OBJ) {
        jv tz = js_get(rt, o, js_atom(rt, "timeZone"));
        (void)tz;
    }
    struct dparts p;
    split_time(t, 1, &p);
    char buf[160];
    unsigned n = 0;
#define PUT(...) n += (unsigned)ksnprintf(buf + n, sizeof(buf) - n, __VA_ARGS__)
    if (!any) {
        if (want_date)
            wd = 0, dd = mm = yy = js_atom(rt, "numeric");
        if (want_time)
            hh = mi = ss = js_atom(rt, "2-digit");
        if (want_date && want_time)
            any = 2;
    }
    int long_month = mm && (js_str_is(mm, "long") || js_str_is(mm, "short"));
    if (wd) {
        int lng = js_str_is(wd, "long");
        PUT("%s", pl ? (lng ? day_pl[p.wday] : day_pl_short[p.wday]) : (lng ? day_en_long[p.wday] : day_en[p.wday]));
        if (dd || mm || yy)
            PUT(", ");
    }
    if (dd || mm || yy) {
        if (pl) {
            if (long_month) {
                if (dd) PUT("%d ", p.day);
                PUT("%s", js_str_is(mm, "short") ? mon_pl_short[p.month] : dd ? mon_pl_gen[p.month] : mon_pl[p.month]);
                if (yy) PUT(" %d", (int)p.year);
            } else {
                int first = 1;
                if (dd) { PUT("%02d", p.day); first = 0; }
                if (mm) { PUT("%s%02d", first ? "" : ".", p.month + 1); first = 0; }
                if (yy) PUT("%s%d", first ? "" : ".", (int)(yy && js_str_is(yy, "2-digit") ? p.year % 100 : p.year));
            }
        } else {
            if (long_month) {
                PUT("%s", js_str_is(mm, "short") ? mon_en[p.month] : mon_en_long[p.month]);
                if (dd) PUT(" %d", p.day);
                if (yy) PUT("%s%d", dd ? ", " : " ", (int)p.year);
            } else {
                int first = 1;
                if (mm) { PUT("%d", p.month + 1); first = 0; }
                if (dd) { PUT("%s%d", first ? "" : "/", p.day); first = 0; }
                if (yy) PUT("%s%d", first ? "" : "/", (int)(js_str_is(yy, "2-digit") ? p.year % 100 : p.year));
            }
        }
    }
    if (hh || mi || ss) {
        if (dd || mm || yy || wd)
            PUT(", ");
        int h = p.hour;
        if (pl) {
            if (hh) PUT("%02d", h);
            if (mi) PUT("%s%02d", hh ? ":" : "", p.min);
            if (ss) PUT(":%02d", p.sec);
        } else {
            int h12 = h % 12 == 0 ? 12 : h % 12;
            if (hh) PUT("%d", h12);
            if (mi) PUT("%s%02d", hh ? ":" : "", p.min);
            if (ss) PUT(":%02d", p.sec);
            if (hh) PUT(" %s", h < 12 ? "AM" : "PM");
        }
    }
#undef PUT
    return jv_str(js_str_utf8(rt, buf, n));
}

NATIVE(dp_to_locale_string) { return date_locale(rt, self, argc, argv, 1, 1); }
NATIVE(dp_to_locale_date_string) { return date_locale(rt, self, argc, argv, 1, 0); }
NATIVE(dp_to_locale_time_string) { return date_locale(rt, self, argc, argv, 0, 1); }
NATIVE(dp_value_of) { return jv_num(this_time(rt, self)); }

/* ---------- Map i Set ---------- */

static uint32_t hash_key(jv k)
{
    switch (k.t) {
    case JT_STR: {
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < k.u.s->len; i++) {
            h ^= k.u.s->c[i];
            h *= 16777619u;
        }
        return h;
    }
    case JT_NUM: {
        double d = k.u.n;
        if (d == 0)
            return 0x12345;
        if (js_isnan(d))
            return 0x54321;
        union { double d; uint64_t u; } x;
        x.d = d;
        return (uint32_t)(x.u ^ (x.u >> 29) ^ (x.u >> 41)) * 2654435761u;
    }
    case JT_OBJ:
        return (uint32_t)(((uint64_t)(uintptr_t)k.u.o >> 4) * 2654435761u);
    case JT_BOOL:
        return 0x777 + k.u.b;
    default:
        return 0x999 + k.t;
    }
}

static void map_reindex(js_rt *rt, jobj *m, uint32_t cap)
{
    uint32_t *idx = js_alloc(rt, sizeof(uint32_t) * cap);
    for (uint32_t i = 0; i < m->u.map.n; i++) {
        if (m->u.map.e[i].deleted)
            continue;
        uint32_t h = hash_key(m->u.map.e[i].key) & (cap - 1);
        while (idx[h])
            h = (h + 1) & (cap - 1);
        idx[h] = i + 1;
    }
    m->u.map.idx = idx;
    m->u.map.icap = cap;
}

static struct jmapent *map_find(jobj *m, jv key)
{
    if (!m->u.map.idx)
        return 0;
    uint32_t mask = m->u.map.icap - 1, h = hash_key(key) & mask;
    for (;;) {
        uint32_t i = m->u.map.idx[h];
        if (!i)
            return 0;
        struct jmapent *e = &m->u.map.e[i - 1];
        if (!e->deleted && js_same_value_zero(e->key, key))
            return e;
        h = (h + 1) & mask;
    }
}

static void map_put(js_rt *rt, jobj *m, jv key, jv value)
{
    if (key.t == JT_NUM && key.u.n == 0)
        key = jv_num(0);                             /* -0 -> 0 */
    struct jmapent *e = map_find(m, key);
    if (e) {
        e->value = value;
        return;
    }
    if (m->u.map.n == m->u.map.cap) {
        uint32_t cap = m->u.map.cap ? m->u.map.cap * 2 : 8;
        struct jmapent *ne = js_alloc(rt, sizeof(struct jmapent) * cap);
        if (m->u.map.n)
            memcpy(ne, m->u.map.e, sizeof(struct jmapent) * m->u.map.n);
        m->u.map.e = ne;
        m->u.map.cap = cap;
    }
    e = &m->u.map.e[m->u.map.n++];
    e->key = key;
    e->value = value;
    e->deleted = 0;
    m->u.map.live++;
    if (!m->u.map.idx || m->u.map.n * 2 > m->u.map.icap) {
        map_reindex(rt, m, m->u.map.icap ? m->u.map.icap * 2 : 16);
    } else {
        uint32_t mask = m->u.map.icap - 1, h = hash_key(key) & mask;
        while (m->u.map.idx[h])
            h = (h + 1) & mask;
        m->u.map.idx[h] = m->u.map.n;
    }
}

static jobj *this_map(js_rt *rt, jv self, int cls)
{
    if (self.t != JT_OBJ || self.u.o->cls != cls)
        js_throw_error(rt, JS_ERR_TYPE, "Method %s.prototype called on incompatible receiver", cls == JC_MAP ? "Map" : "Set");
    return self.u.o;
}

static jv map_ctor_common(js_rt *rt, int argc, jv *argv, int cls, jobj *proto)
{
    jobj *m = js_new_object(rt, proto);
    m->cls = (uint8_t)cls;
    jv src = ARG(0);
    if (!IS_NULLISH(src)) {
        jobj *items = js_iterate_to_array(rt, src).u.o;
        for (uint32_t i = 0; i < items->u.arr.len; i++) {
            jv it = items->u.arr.v[i];
            if (cls == JC_MAP) {
                if (it.t != JT_OBJ)
                    js_throw_error(rt, JS_ERR_TYPE, "Iterator value is not an entry object");
                map_put(rt, m, js_get_index(rt, it, jv_num(0)), js_get_index(rt, it, jv_num(1)));
            } else {
                map_put(rt, m, it, it);
            }
        }
    }
    return jv_obj(m);
}

static jv map_ctor(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn U) { return map_ctor_common(rt, argc, argv, JC_MAP, rt->map_proto); }
static jv set_ctor(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn U) { return map_ctor_common(rt, argc, argv, JC_SET, rt->set_proto); }
static jv need_new(js_rt *rt, jv self U, int argc U, jv *argv U, jobj *fn)
{
    char name[40];
    js_utf8(js_func_name(rt, fn), name, sizeof(name));
    js_throw_error(rt, JS_ERR_TYPE, "Constructor %s requires 'new'", name);
}

NATIVE(mapp_get)
{
    struct jmapent *e = map_find(this_map(rt, self, JC_MAP), ARG(0));
    return e ? e->value : jv_undef();
}

NATIVE(mapp_set)
{
    map_put(rt, this_map(rt, self, JC_MAP), ARG(0), ARG(1));
    return self;
}

NATIVE(setp_add)
{
    map_put(rt, this_map(rt, self, JC_SET), ARG(0), ARG(0));
    return self;
}

static jobj *any_map(js_rt *rt, jv self)
{
    if (self.t != JT_OBJ || (self.u.o->cls != JC_MAP && self.u.o->cls != JC_SET))
        js_throw_error(rt, JS_ERR_TYPE, "Method called on incompatible receiver");
    return self.u.o;
}

NATIVE(mapp_has) { return jv_bool(map_find(any_map(rt, self), ARG(0)) != 0); }

NATIVE(mapp_delete)
{
    jobj *m = any_map(rt, self);
    struct jmapent *e = map_find(m, ARG(0));
    if (!e)
        return jv_bool(0);
    e->deleted = 1;
    e->key = e->value = jv_undef();
    m->u.map.live--;
    return jv_bool(1);
}

NATIVE(mapp_clear)
{
    jobj *m = any_map(rt, self);
    for (uint32_t i = 0; i < m->u.map.n; i++)
        m->u.map.e[i].deleted = 1;
    m->u.map.live = 0;
    m->u.map.n = 0;
    m->u.map.idx = 0;
    m->u.map.icap = 0;
    return jv_undef();
}

NATIVE(mapp_size) { return jv_num(any_map(rt, self)->u.map.live); }

NATIVE(mapp_for_each)
{
    jobj *m = any_map(rt, self);
    jv f = ARG(0);
    if (!js_is_callable(f))
        js_throw_error(rt, JS_ERR_TYPE, "callback is not a function");
    for (uint32_t i = 0; i < m->u.map.n; i++) {
        struct jmapent *e = &m->u.map.e[i];
        if (e->deleted)
            continue;
        jv args[3] = { e->value, e->key, self };
        js_call(rt, f, ARG(1), 3, args);
    }
    return jv_undef();
}

enum { MI_KEYS, MI_VALUES, MI_ENTRIES };

static jv map_items(js_rt *rt, jv self, int kind)
{
    jobj *m = any_map(rt, self), *out = js_new_array(rt, 0);
    for (uint32_t i = 0; i < m->u.map.n; i++) {
        struct jmapent *e = &m->u.map.e[i];
        if (e->deleted)
            continue;
        if (kind == MI_KEYS) {
            js_array_push(rt, out, e->key);
        } else if (kind == MI_VALUES) {
            js_array_push(rt, out, e->value);
        } else {
            jobj *pair = js_new_array(rt, 0);
            js_array_push(rt, pair, e->key);
            js_array_push(rt, pair, e->value);
            js_array_push(rt, out, jv_obj(pair));
        }
    }
    return jv_obj(out);
}

NATIVE(mapp_keys) { return jv_obj(js_make_iter(rt, map_items(rt, self, MI_KEYS).u.o, IT_VALUES)); }
NATIVE(mapp_values) { return jv_obj(js_make_iter(rt, map_items(rt, self, MI_VALUES).u.o, IT_VALUES)); }
NATIVE(mapp_entries) { return jv_obj(js_make_iter(rt, map_items(rt, self, MI_ENTRIES).u.o, IT_VALUES)); }

/* ---------- iteratory (next) ----------
 * tablica.values(), map.keys(), napis[Symbol.iterator]() ... zwracaja obiekt
 * z metoda next() -> { value, done }. Strony (kod po Babel/TypeScript) wolaja
 * next() bezposrednio, wiec zwykla tablica tu nie wystarcza. */

jobj *js_make_iter(js_rt *rt, jobj *src, int kind)
{
    jobj *it = js_new_object(rt, rt->iterator_proto);
    it->cls = JC_ITER;
    it->u.it.src = src;
    it->u.it.pos = 0;
    it->u.it.kind = (uint8_t)kind;
    it->u.it.done = 0;
    return it;
}

/* Nastepny element: 1 = jest (w *out), 0 = koniec */
static int iter_step(js_rt *rt, jobj *it, jv *out)
{
    jobj *a = it->u.it.src;
    if (it->u.it.done || it->u.it.pos >= a->u.arr.len) {
        it->u.it.done = 1;
        return 0;
    }
    uint32_t i = it->u.it.pos++;
    if (it->u.it.kind == IT_KEYS) {
        *out = jv_num(i);
    } else if (it->u.it.kind == IT_VALUES) {
        *out = a->u.arr.v[i];
    } else {
        jobj *pair = js_new_array(rt, 0);
        js_array_push(rt, pair, jv_num(i));
        js_array_push(rt, pair, a->u.arr.v[i]);
        *out = jv_obj(pair);
    }
    return 1;
}

NATIVE(iter_next)
{
    if (self.t != JT_OBJ || self.u.o->cls != JC_ITER)
        js_throw_error(rt, JS_ERR_TYPE, "next method called on incompatible receiver");
    jv v = jv_undef();
    int has = iter_step(rt, self.u.o, &v);
    jobj *r = js_new_object(rt, rt->object_proto);
    js_set(rt, jv_obj(r), rt->A.value, v);
    js_set(rt, jv_obj(r), rt->A.done, jv_bool(!has));
    return jv_obj(r);
}

NATIVE(iter_self) { return self; }

/* ---------- iteracja (for-of, ...) ---------- */

jv js_iterate_to_array(js_rt *rt, jv v)
{
    if (v.t == JT_STR) {
        jobj *out = js_new_array(rt, 0);
        jstr *s = v.u.s;
        for (uint32_t i = 0; i < s->len; i++) {
            uint32_t n = 1;
            if (s->c[i] >= 0xD800 && s->c[i] < 0xDC00 && i + 1 < s->len && s->c[i + 1] >= 0xDC00 && s->c[i + 1] < 0xE000)
                n = 2;
            js_array_push(rt, out, jv_str(js_substr(rt, s, i, i + n)));
            i += n - 1;
        }
        return jv_obj(out);
    }
    if (v.t == JT_OBJ) {
        jobj *o = v.u.o;
        switch (o->cls) {
        case JC_ARRAY:
            return v;
        case JC_ARGS: {
            jobj *out = js_new_array(rt, o->u.arr.len);
            memcpy(out->u.arr.v, o->u.arr.v, sizeof(jv) * o->u.arr.len);
            return jv_obj(out);
        }
        case JC_MAP:
            return map_items(rt, v, MI_ENTRIES);
        case JC_SET:
            return map_items(rt, v, MI_VALUES);
        case JC_STR:
            return js_iterate_to_array(rt, o->u.prim);
        case JC_GEN: {                               /* generator: next() az do konca */
            jobj *out = js_new_array(rt, 0);
            for (;;) {
                int done;
                js_step(rt);
                jv x = js_gen_resume(rt, o, 1, jv_undef(), &done);
                if (done)
                    break;
                js_array_push(rt, out, x);
            }
            return jv_obj(out);
        }
        case JC_ITER: {                              /* reszta iteratora - bez wolania next() z JS */
            jobj *out = js_new_array(rt, 0);
            jv x;
            while (iter_step(rt, o, &x))
                js_array_push(rt, out, x);
            return jv_obj(out);
        }
        }
        jv itf = js_get(rt, v, rt->sym_iterator);
        if (js_is_callable(itf)) {
            jv it = js_call(rt, itf, v, 0, 0);
            if (it.t == JT_OBJ && it.u.o->cls == JC_ARRAY)
                return it;
            if (it.t == JT_OBJ && it.u.o->cls == JC_ITER && js_get(rt, it, rt->A.next).t == JT_OBJ &&
                js_get(rt, it, rt->A.next).u.o->cls == JC_NATIVE && js_get(rt, it, rt->A.next).u.o->u.nf.fn == iter_next)
                return js_iterate_to_array(rt, it);
            jv next = js_get(rt, it, rt->A.next);
            if (!js_is_callable(next))
                js_throw_error(rt, JS_ERR_TYPE, "Result of the Symbol.iterator method is not an object");
            jobj *out = js_new_array(rt, 0);
            for (;;) {
                js_step(rt);
                jv r = js_call(rt, next, it, 0, 0);
                if (r.t != JT_OBJ)
                    js_throw_error(rt, JS_ERR_TYPE, "Iterator result is not an object");
                if (js_truthy(js_get(rt, r, rt->A.done)))
                    break;
                js_array_push(rt, out, js_get(rt, r, rt->A.value));
            }
            return jv_obj(out);
        }
    }
    js_throw_error(rt, JS_ERR_TYPE, "%s is not iterable", v.t == JT_OBJ ? "object" : js_typeof(v));
}

/* ---------- Promise ---------- */

struct jreaction {
    jv on_ful, on_rej;
    jobj *derived;
    struct jreaction *next;
};

static void enqueue(js_rt *rt, void (*run)(js_rt *, struct jmicro *), int kind, jv a, jv b, jobj *p)
{
    struct jmicro *m = js_alloc(rt, sizeof(*m));
    m->run = run;
    m->kind = kind;
    m->a = a;
    m->b = b;
    m->p = p;
    if (rt->micro_tail)
        rt->micro_tail->next = m;
    else
        rt->micro_head = m;
    rt->micro_tail = m;
}

void js_run_microtasks(js_rt *rt)
{
    while (rt->micro_head) {
        struct jmicro *m = rt->micro_head;
        rt->micro_head = m->next;
        if (!rt->micro_head)
            rt->micro_tail = 0;
        js_step(rt);
        m->run(rt, m);
    }
}

jobj *js_new_promise(js_rt *rt)
{
    jobj *p = js_new_object(rt, rt->promise_proto);
    p->cls = JC_PROMISE;
    return p;
}

static void reaction_job(js_rt *rt, struct jmicro *m)
{
    jv handler = m->a, value = m->b;
    jobj *derived = m->p;
    if (js_is_callable(handler)) {
        jv r;
        if (js_try_call(rt, handler, jv_undef(), 1, &value, &r)) {
            if (derived)
                js_promise_reject(rt, derived, r);
        } else if (derived) {
            js_promise_resolve(rt, derived, r);
        }
    } else if (derived) {
        if (m->kind == 1)
            js_promise_resolve(rt, derived, value);
        else
            js_promise_reject(rt, derived, value);
    }
}

static void settle(js_rt *rt, jobj *p, int state, jv v)
{
    if (p->u.pr.state)
        return;
    p->u.pr.state = state;
    p->u.pr.value = v;
    for (struct jreaction *r = p->u.pr.reactions; r; r = r->next)
        enqueue(rt, reaction_job, state, state == 1 ? r->on_ful : r->on_rej, v, r->derived);
    p->u.pr.reactions = p->u.pr.last = 0;
    if (state == 2 && !p->u.pr.handled && rt->nunhandled < 16)
        rt->unhandled[rt->nunhandled++] = p;   /* zapiszemy w konsoli, jesli nikt nie doda .catch */
}

void js_promise_reject(js_rt *rt, jobj *p, jv v)
{
    settle(rt, p, 2, v);
}

/* Funkcje resolve/reject przekazywane do new Promise((resolve, reject) => ...) */
static jv resolving_fn(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn)
{
    jobj *rec = fn->u.nf.data.u.o;                 /* rec->u.pr.value = obietnica, state = juz uzyte */
    if (rec->u.pr.state)
        return jv_undef();
    rec->u.pr.state = 1;
    jobj *p = rec->u.pr.value.u.o;
    if (fn->u.nf.len == 0)
        js_promise_resolve(rt, p, ARG(0));
    else
        js_promise_reject(rt, p, ARG(0));
    return jv_undef();
}

static void make_resolvers(js_rt *rt, jobj *p, jobj **res, jobj **rej)
{
    jobj *rec = js_new_object(rt, 0);
    rec->u.pr.value = jv_obj(p);
    *res = js_new_native(rt, "resolve", resolving_fn, 0);
    (*res)->u.nf.data = jv_obj(rec);
    *rej = js_new_native(rt, "reject", resolving_fn, 1);
    (*rej)->u.nf.data = jv_obj(rec);
    (*rej)->u.nf.len = 1;
    (*res)->u.nf.len = 0;
}

static void thenable_job(js_rt *rt, struct jmicro *m)
{
    jobj *res, *rej;
    make_resolvers(rt, m->p, &res, &rej);
    jv args[2] = { jv_obj(res), jv_obj(rej) }, r;
    if (js_try_call(rt, m->a, m->b, 2, args, &r))
        js_call(rt, jv_obj(rej), jv_undef(), 1, &r);
}

void js_promise_resolve(js_rt *rt, jobj *p, jv v)
{
    if (p->u.pr.state)
        return;
    if (v.t == JT_OBJ && v.u.o == p) {
        js_promise_reject(rt, p, jv_obj(js_make_error(rt, JS_ERR_TYPE, S(rt, "Chaining cycle detected for promise"))));
        return;
    }
    if (v.t == JT_OBJ) {
        jv then;
        jv getter_args[1];
        (void)getter_args;
        if (v.u.o->cls == JC_PROMISE && v.u.o->proto == rt->promise_proto) {
            then = js_get(rt, jv_obj(rt->promise_proto), rt->A.then);
        } else {
            then = js_get(rt, v, rt->A.then);
        }
        if (js_is_callable(then)) {
            enqueue(rt, thenable_job, 0, then, v, p);
            return;
        }
    }
    settle(rt, p, 1, v);
}

static jv promise_ctor(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn U)
{
    jv exec = ARG(0);
    if (!js_is_callable(exec))
        js_throw_error(rt, JS_ERR_TYPE, "Promise resolver is not a function");
    jobj *p = js_new_promise(rt), *res, *rej;
    make_resolvers(rt, p, &res, &rej);
    jv args[2] = { jv_obj(res), jv_obj(rej) }, r;
    if (js_try_call(rt, exec, jv_undef(), 2, args, &r))
        js_call(rt, jv_obj(rej), jv_undef(), 1, &r);
    return jv_obj(p);
}

static jobj *add_reaction(js_rt *rt, jobj *p, jv on_ful, jv on_rej)
{
    jobj *derived = js_new_promise(rt);
    p->u.pr.handled = 1;
    if (p->u.pr.state) {
        enqueue(rt, reaction_job, p->u.pr.state, p->u.pr.state == 1 ? on_ful : on_rej, p->u.pr.value, derived);
    } else {
        struct jreaction *r = js_alloc(rt, sizeof(*r));
        r->on_ful = on_ful;
        r->on_rej = on_rej;
        r->derived = derived;
        if (p->u.pr.last)
            p->u.pr.last->next = r;
        else
            p->u.pr.reactions = r;
        p->u.pr.last = r;
    }
    return derived;
}

jobj *js_promise_then(js_rt *rt, jobj *p, jv on_ful, jv on_rej)
{
    return add_reaction(rt, p, on_ful, on_rej);
}

static jobj *this_promise(js_rt *rt, jv self)
{
    if (self.t != JT_OBJ || self.u.o->cls != JC_PROMISE)
        js_throw_error(rt, JS_ERR_TYPE, "Method Promise.prototype.then called on incompatible receiver");
    return self.u.o;
}

NATIVE(pp_then) { return jv_obj(add_reaction(rt, this_promise(rt, self), ARG(0), ARG(1))); }
NATIVE(pp_catch) { return jv_obj(add_reaction(rt, this_promise(rt, self), jv_undef(), ARG(0))); }

static jv finally_ful(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn)
{
    js_call(rt, fn->u.nf.data, jv_undef(), 0, 0);
    return ARG(0);
}

static jv finally_rej(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn)
{
    js_call(rt, fn->u.nf.data, jv_undef(), 0, 0);
    js_throw(rt, ARG(0));
}

NATIVE(pp_finally)
{
    jobj *p = this_promise(rt, self);
    jv f = ARG(0);
    if (!js_is_callable(f))
        return jv_obj(add_reaction(rt, p, f, f));
    jobj *a = js_new_native(rt, "", finally_ful, 1), *b = js_new_native(rt, "", finally_rej, 1);
    a->u.nf.data = f;
    b->u.nf.data = f;
    return jv_obj(add_reaction(rt, p, jv_obj(a), jv_obj(b)));
}

NATIVE(promise_resolve)
{
    jv v = ARG(0);
    if (v.t == JT_OBJ && v.u.o->cls == JC_PROMISE)
        return v;
    jobj *p = js_new_promise(rt);
    js_promise_resolve(rt, p, v);
    return jv_obj(p);
}

NATIVE(promise_reject)
{
    jobj *p = js_new_promise(rt);
    js_promise_reject(rt, p, ARG(0));
    return jv_obj(p);
}

/* Promise.all / allSettled: rec = [wyniki, obietnica-wynik, licznik(obiekt), indeks] */
enum { AGG_ALL, AGG_SETTLED_OK, AGG_SETTLED_ERR };

static jv agg_element(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn)
{
    jobj *rec = fn->u.nf.data.u.o;
    jobj *values = rec->u.arr.v[0].u.o, *result = rec->u.arr.v[1].u.o, *counter = rec->u.arr.v[2].u.o;
    uint32_t idx = (uint32_t)rec->u.arr.v[3].u.n;
    if (rec->u.arr.v[4].t == JT_BOOL)
        return jv_undef();                           /* juz wywolana */
    rec->u.arr.v[4] = jv_bool(1);
    jv v = ARG(0);
    int kind = fn->u.nf.len;
    if (kind != AGG_ALL) {
        jobj *o = js_new_object(rt, rt->object_proto);
        js_set(rt, jv_obj(o), rt->A.status, jv_str(js_atom(rt, kind == AGG_SETTLED_OK ? "fulfilled" : "rejected")));
        js_set(rt, jv_obj(o), kind == AGG_SETTLED_OK ? rt->A.value : rt->A.reason, v);
        v = jv_obj(o);
    }
    values->u.arr.v[idx] = v;
    if (--counter->u.prim.u.n == 0)
        js_promise_resolve(rt, result, jv_obj(values));
    return jv_undef();
}

static jv promise_aggregate(js_rt *rt, jv list, int settled)
{
    jobj *items = js_iterate_to_array(rt, list).u.o;
    jobj *result = js_new_promise(rt), *values = js_new_array(rt, items->u.arr.len);
    jobj *counter = js_new_object(rt, 0);
    counter->u.prim = jv_num(items->u.arr.len);
    if (!items->u.arr.len) {
        js_promise_resolve(rt, result, jv_obj(values));
        return jv_obj(result);
    }
    jobj *res, *rej;
    make_resolvers(rt, result, &res, &rej);
    for (uint32_t i = 0; i < items->u.arr.len; i++) {
        jv pv = promise_resolve(rt, jv_undef(), 1, &items->u.arr.v[i], 0);
        jobj *rec = js_new_array(rt, 5);
        rec->u.arr.v[0] = jv_obj(values);
        rec->u.arr.v[1] = jv_obj(result);
        rec->u.arr.v[2] = jv_obj(counter);
        rec->u.arr.v[3] = jv_num(i);
        jobj *ok = js_new_native(rt, "", agg_element, 1);
        ok->u.nf.data = jv_obj(rec);
        ok->u.nf.len = settled ? AGG_SETTLED_OK : AGG_ALL;
        jv on_rej = jv_obj(rej);
        if (settled) {
            jobj *bad = js_new_native(rt, "", agg_element, 1);
            bad->u.nf.data = jv_obj(rec);
            bad->u.nf.len = AGG_SETTLED_ERR;
            on_rej = jv_obj(bad);
        }
        jv then = js_get(rt, pv, rt->A.then);
        jv args[2] = { jv_obj(ok), on_rej };
        js_call(rt, then, pv, 2, args);
    }
    return jv_obj(result);
}

NATIVE(promise_all) { return promise_aggregate(rt, ARG(0), 0); }
NATIVE(promise_all_settled) { return promise_aggregate(rt, ARG(0), 1); }

NATIVE(promise_race)
{
    jobj *items = js_iterate_to_array(rt, ARG(0)).u.o;
    jobj *result = js_new_promise(rt), *res, *rej;
    make_resolvers(rt, result, &res, &rej);
    for (uint32_t i = 0; i < items->u.arr.len; i++) {
        jv pv = promise_resolve(rt, jv_undef(), 1, &items->u.arr.v[i], 0);
        jv args[2] = { jv_obj(res), jv_obj(rej) };
        js_call(rt, js_get(rt, pv, rt->A.then), pv, 2, args);
    }
    return jv_obj(result);
}

static void call_job(js_rt *rt, struct jmicro *m)
{
    jv r;
    if (js_try_call(rt, m->a, jv_undef(), 0, 0, &r)) {
        rt->exc = r;
        js_report_exception(rt);
    }
}

NATIVE(g_queue_microtask)
{
    if (!js_is_callable(ARG(0)))
        js_throw_error(rt, JS_ERR_TYPE, "The callback provided as parameter 1 is not a function.");
    enqueue(rt, call_job, 0, argv[0], jv_undef(), 0);
    return jv_undef();
}

NATIVE(g_structured_clone)
{
    jv v = ARG(0);
    if (v.t != JT_OBJ)
        return v;
    jstr *s = js_json_stringify(rt, v, jv_undef());
    if (!s)
        return jv_undef();
    jv parse = js_get(rt, js_get(rt, jv_obj(rt->global), js_atom(rt, "JSON")), js_atom(rt, "parse"));
    jv a = jv_str(s);
    return js_call(rt, parse, jv_undef(), 1, &a);
}

/* ---------- bledy dla przegladarki ---------- */

jstr *js_error_text(js_rt *rt, jv exc)
{
    if (exc.t == JT_OBJ && exc.u.o->cls == JC_ERROR) {
        struct jprop *n = js_find(exc.u.o, rt->A.name), *m = js_find(exc.u.o, rt->A.message);
        jstr *name = n && n->v.t == JT_STR ? n->v.u.s : js_atom(rt, "Error");
        jstr *msg = m && m->v.t == JT_STR ? m->v.u.s : rt->empty;
        if (!msg->len)
            return name;
        return js_concat(rt, js_concat(rt, name, S(rt, ": ")), msg);
    }
    return js_inspect(rt, exc, 1);
}

void js_report_exception(js_rt *rt)
{
    char msg[240];
    if (rt->aborting && rt->abort_msg) {
        ksnprintf(rt->last_error, sizeof(rt->last_error), "%s (%s, linia %u)", rt->abort_msg,
                  rt->script_name ? rt->script_name : "skrypt", rt->exc_line);
    } else {
        int saved = rt->aborting;
        rt->aborting = 0;
        js_utf8(js_error_text(rt, rt->exc), msg, sizeof(msg));
        rt->aborting = saved;
        ksnprintf(rt->last_error, sizeof(rt->last_error), "Uncaught %s (%s, linia %u)", msg,
                  rt->script_name ? rt->script_name : "skrypt", rt->exc_line);
    }
    if (rt->host.console)
        rt->host.console(rt->host.ctx, 2, rt->last_error);
}

/* Odrzucone obietnice, do ktorych nikt nie dodal .catch - jak "Uncaught (in promise)" w przegladarce */
void js_report_unhandled(js_rt *rt)
{
    int n = rt->nunhandled;
    rt->nunhandled = 0;
    for (int i = 0; i < n; i++) {
        jobj *p = rt->unhandled[i];
        if (p->u.pr.handled)
            continue;
        p->u.pr.handled = 1;
        char msg[240];
        js_utf8(js_error_text(rt, p->u.pr.value), msg, sizeof(msg));
        ksnprintf(rt->last_error, sizeof(rt->last_error), "Uncaught (in promise) %s (%s)", msg,
                  rt->script_name ? rt->script_name : "skrypt");
        if (rt->host.console)
            rt->host.console(rt->host.ctx, 2, rt->last_error);
    }
}

/* ---------- start ---------- */

static jobj *make_ctor(js_rt *rt, const char *name, jnative call, jnative ctor, int len, jobj *proto)
{
    jobj *f = js_new_native(rt, name, call, len);
    f->u.nf.ctor = ctor;
    js_define(rt, f, rt->A.prototype, jv_obj(proto), 0);
    js_define(rt, proto, rt->A.constructor, jv_obj(f), PF_WRITE | PF_CONF);
    js_def_val(rt, rt->global, name, jv_obj(f));
    return f;
}

void js_lib2_init(js_rt *rt)
{
    jobj *G = rt->global;

    /* wspolny prototyp iteratorow: next() i [Symbol.iterator]() { return this } */
    rt->iterator_proto = js_new_object(rt, rt->object_proto);
    js_gen_init(rt);
    js_def_fn(rt, rt->iterator_proto, "next", iter_next, 0);
    js_define(rt, rt->iterator_proto, rt->sym_iterator,
              jv_obj(js_new_native(rt, "[Symbol.iterator]", iter_self, 0)), PF_WRITE | PF_CONF);

    /* String */
    jobj *sp = rt->string_proto;
    sp->cls = JC_STR;
    sp->u.prim = jv_str(rt->empty);
    jobj *Str = make_ctor(rt, "String", string_call, string_ctor, 1, sp);
    js_def_fn(rt, Str, "fromCharCode", str_from_char_code, 1);
    js_def_fn(rt, Str, "fromCodePoint", str_from_code_point, 1);
    js_def_fn(rt, Str, "raw", str_raw, 1);
    static const struct { const char *n; jnative f; int len; } sf[] = {
        { "toString", strp_to_string, 0 }, { "valueOf", strp_to_string, 0 }, { "charAt", strp_char_at, 1 },
        { "charCodeAt", strp_char_code_at, 1 }, { "codePointAt", strp_code_point_at, 1 }, { "at", strp_at, 1 },
        { "indexOf", strp_index_of, 1 }, { "lastIndexOf", strp_last_index_of, 1 }, { "includes", strp_includes, 1 },
        { "startsWith", strp_starts_with, 1 }, { "endsWith", strp_ends_with, 1 }, { "slice", strp_slice, 2 },
        { "substring", strp_substring, 2 }, { "substr", strp_substr, 2 }, { "toUpperCase", strp_upper, 0 },
        { "toLowerCase", strp_lower, 0 }, { "toLocaleUpperCase", strp_upper, 0 },
        { "toLocaleLowerCase", strp_lower, 0 }, { "trim", strp_trim, 0 }, { "trimStart", strp_trim_start, 0 },
        { "trimEnd", strp_trim_end, 0 }, { "trimLeft", strp_trim_start, 0 }, { "trimRight", strp_trim_end, 0 },
        { "padStart", strp_pad_start, 2 }, { "padEnd", strp_pad_end, 2 }, { "repeat", strp_repeat, 1 },
        { "concat", strp_concat, 1 }, { "localeCompare", strp_locale_compare, 1 },
        { "normalize", strp_normalize, 0 },
    };
    for (unsigned i = 0; i < sizeof(sf) / sizeof(sf[0]); i++)
        js_def_fn(rt, sp, sf[i].n, sf[i].f, sf[i].len);
    js_define(rt, sp, rt->sym_iterator, jv_obj(js_new_native(rt, "[Symbol.iterator]", strp_iterator, 0)), PF_WRITE | PF_CONF);

    /* Date */
    jobj *dp = rt->date_proto;
    jobj *D = make_ctor(rt, "Date", date_call, date_ctor, 7, dp);
    js_def_fn(rt, D, "now", date_now, 0);
    js_def_fn(rt, D, "parse", date_parse, 1);
    js_def_fn(rt, D, "UTC", date_utc, 7);
    static const struct { const char *n; jnative f; int len; } df[] = {
        { "getFullYear", dp_get_full_year, 0 }, { "getMonth", dp_get_month, 0 }, { "getDate", dp_get_date, 0 },
        { "getDay", dp_get_day, 0 }, { "getHours", dp_get_hours, 0 }, { "getMinutes", dp_get_minutes, 0 },
        { "getSeconds", dp_get_seconds, 0 }, { "getMilliseconds", dp_get_ms, 0 },
        { "getUTCFullYear", dp_get_utc_full_year, 0 }, { "getUTCMonth", dp_get_utc_month, 0 },
        { "getUTCDate", dp_get_utc_date, 0 }, { "getUTCDay", dp_get_utc_day, 0 },
        { "getUTCHours", dp_get_utc_hours, 0 }, { "getUTCMinutes", dp_get_utc_minutes, 0 },
        { "getUTCSeconds", dp_get_utc_seconds, 0 }, { "getUTCMilliseconds", dp_get_utc_ms, 0 },
        { "getYear", dp_get_year, 0 }, { "getTime", dp_get_time, 0 }, { "valueOf", dp_value_of, 0 },
        { "getTimezoneOffset", dp_get_timezone_offset, 0 }, { "setTime", dp_set_time, 1 },
        { "setFullYear", dp_set_full_year, 3 }, { "setMonth", dp_set_month, 2 }, { "setDate", dp_set_date, 1 },
        { "setHours", dp_set_hours, 4 }, { "setMinutes", dp_set_minutes, 3 },
        { "setSeconds", dp_set_seconds, 2 }, { "setMilliseconds", dp_set_ms, 1 },
        { "setUTCFullYear", dp_set_utc_full_year, 3 }, { "setUTCMonth", dp_set_utc_month, 2 },
        { "setUTCDate", dp_set_utc_date, 1 }, { "setUTCHours", dp_set_utc_hours, 4 },
        { "setUTCMinutes", dp_set_utc_minutes, 3 }, { "setUTCSeconds", dp_set_utc_seconds, 2 },
        { "toISOString", dp_to_iso_string, 0 }, { "toJSON", dp_to_json, 1 }, { "toString", dp_to_string, 0 },
        { "toDateString", dp_to_date_string, 0 }, { "toTimeString", dp_to_time_string, 0 },
        { "toUTCString", dp_to_utc_string, 0 }, { "toGMTString", dp_to_utc_string, 0 },
        { "toLocaleString", dp_to_locale_string, 0 }, { "toLocaleDateString", dp_to_locale_date_string, 0 },
        { "toLocaleTimeString", dp_to_locale_time_string, 0 },
    };
    for (unsigned i = 0; i < sizeof(df) / sizeof(df[0]); i++)
        js_def_fn(rt, dp, df[i].n, df[i].f, df[i].len);

    /* Map, Set, WeakMap, WeakSet */
    jobj *mp = rt->map_proto, *stp = rt->set_proto;
    make_ctor(rt, "Map", need_new, map_ctor, 0, mp);
    make_ctor(rt, "Set", need_new, set_ctor, 0, stp);
    jobj *wm = js_new_object(rt, mp), *ws = js_new_object(rt, stp);
    make_ctor(rt, "WeakMap", need_new, map_ctor, 0, wm);
    make_ctor(rt, "WeakSet", need_new, set_ctor, 0, ws);
    js_def_fn(rt, mp, "get", mapp_get, 1);
    js_def_fn(rt, mp, "set", mapp_set, 2);
    js_def_fn(rt, mp, "has", mapp_has, 1);
    js_def_fn(rt, mp, "delete", mapp_delete, 1);
    js_def_fn(rt, mp, "clear", mapp_clear, 0);
    js_def_fn(rt, mp, "forEach", mapp_for_each, 1);
    js_def_fn(rt, mp, "keys", mapp_keys, 0);
    js_def_fn(rt, mp, "values", mapp_values, 0);
    js_def_fn(rt, mp, "entries", mapp_entries, 0);
    js_def_getset(rt, mp, "size", mapp_size, 0);
    js_define(rt, mp, rt->sym_iterator, js_get(rt, jv_obj(mp), js_atom(rt, "entries")), PF_WRITE | PF_CONF);
    js_def_fn(rt, stp, "add", setp_add, 1);
    js_def_fn(rt, stp, "has", mapp_has, 1);
    js_def_fn(rt, stp, "delete", mapp_delete, 1);
    js_def_fn(rt, stp, "clear", mapp_clear, 0);
    js_def_fn(rt, stp, "forEach", mapp_for_each, 1);
    js_def_fn(rt, stp, "values", mapp_values, 0);
    js_def_fn(rt, stp, "keys", mapp_values, 0);
    js_def_fn(rt, stp, "entries", mapp_entries, 0);
    js_def_getset(rt, stp, "size", mapp_size, 0);
    js_define(rt, stp, rt->sym_iterator, js_get(rt, jv_obj(stp), js_atom(rt, "values")), PF_WRITE | PF_CONF);

    /* Promise */
    jobj *pp = rt->promise_proto;
    jobj *P = make_ctor(rt, "Promise", need_new, promise_ctor, 1, pp);
    rt->promise_ctor = P;
    js_def_fn(rt, pp, "then", pp_then, 2);
    js_def_fn(rt, pp, "catch", pp_catch, 1);
    js_def_fn(rt, pp, "finally", pp_finally, 1);
    js_def_fn(rt, P, "resolve", promise_resolve, 1);
    js_def_fn(rt, P, "reject", promise_reject, 1);
    js_def_fn(rt, P, "all", promise_all, 1);
    js_def_fn(rt, P, "allSettled", promise_all_settled, 1);
    js_def_fn(rt, P, "race", promise_race, 1);
    js_def_fn(rt, G, "queueMicrotask", g_queue_microtask, 1);
    js_def_fn(rt, G, "structuredClone", g_structured_clone, 1);

    js_regex_string_init(rt);
}

/* Rdzen JavaScriptu: pamiec, teksty, atomy, obiekty i ich wlasciwosci,
 * zamiany typow (ToNumber, ToString...), bledy. Interpreter (js_eval.c)
 * i obiekty wbudowane (js_lib.c) buduja na tym.
 *
 * Obiekt to lista wlasciwosci (nazwa -> wartosc) i wskaznik na prototyp:
 * gdy obiekt nie ma wlasciwosci, szukamy jej w prototypie, potem w jego
 * prototypie itd. Tak [1,2].push znajduje push w Array.prototype.
 * Obiekty z wieloma wlasciwosciami (window, prototypy) maja dodatkowo
 * tablice mieszajaca, zeby nie przegladac ich po kolei. */

#include "js_int.h"
#include "string.h"
#include "utf8.h"
#include "timer.h"
#include "thread.h"
#include <stdarg.h>

/* ---------- pamiec ---------- */

void *js_alloc(js_rt *rt, size_t n)
{
    void *p = arena_alloc(&rt->mem, n);
    if ((!p || rt->mem.used > rt->mem_limit) && rt->catch_top && !rt->aborting) {
        rt->dead = 1;
        js_abort(rt, "Brak pamieci dla JavaScriptu - skrypt przerwany");
    }
    return p;
}

/* ---------- teksty ---------- */

jstr *js_str_new(js_rt *rt, uint32_t len)
{
    jstr *s = js_alloc(rt, sizeof(jstr) + (size_t)len * 2 + 2);
    s->c = (uint16_t *)(s + 1);
    s->len = len;
    return s;
}

jstr *js_str_utf16(js_rt *rt, const uint16_t *c, uint32_t len)
{
    jstr *s = js_str_new(rt, len);
    memcpy(s->c, c, (size_t)len * 2);
    return s;
}

jstr *js_str_utf8(js_rt *rt, const char *s, uint32_t len)
{
    const char *p = s, *end = s + len;
    uint32_t n = 0;
    int ascii = 1;
    for (uint32_t i = 0; i < len; i++)
        if ((uint8_t)s[i] >= 0x80) {
            ascii = 0;
            break;
        }
    if (ascii) {
        jstr *r = js_str_new(rt, len);
        for (uint32_t i = 0; i < len; i++)
            r->c[i] = (uint8_t)s[i];
        return r;
    }
    while (p < end) {
        uint32_t cp = utf8_next(&p, end);
        n += cp >= 0x10000 ? 2 : 1;
    }
    jstr *r = js_str_new(rt, n);
    p = s;
    n = 0;
    while (p < end) {
        uint32_t cp = utf8_next(&p, end);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            r->c[n++] = (uint16_t)(0xD800 + (cp >> 10));
            r->c[n++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            r->c[n++] = (uint16_t)cp;
        }
    }
    return r;
}

jstr *js_str_c(js_rt *rt, const char *s)
{
    return js_str_utf8(rt, s, (uint32_t)strlen(s));
}

#define STR_MAX (1u << 28)

jstr *js_concat(js_rt *rt, jstr *a, jstr *b)
{
    if (!a->len)
        return b;
    if (!b->len)
        return a;
    uint64_t n = (uint64_t)a->len + b->len;
    if (n > STR_MAX)
        js_throw_error(rt, JS_ERR_RANGE, "Invalid string length");
    struct jsbuf *B = a->buf;
    jstr *r;
    /* a konczy sie tam, gdzie zajeta czesc bufora - dopisujemy b bez kopiowania a */
    if (B && a->c + a->len == B->data + B->used && B->used + b->len <= B->cap) {
        memcpy(B->data + B->used, b->c, (size_t)b->len * 2);
        B->used += b->len;
        r = js_alloc(rt, sizeof(jstr));
        r->c = a->c;
        r->len = (uint32_t)n;
        r->buf = B;
        return r;
    }
    uint32_t cap = (uint32_t)(n < 16 ? 32 : n < 256 ? n + 32 : n * 2);
    if (cap > STR_MAX)
        cap = (uint32_t)n;
    B = js_alloc(rt, sizeof(struct jsbuf) + (size_t)cap * 2);
    B->cap = cap;
    B->used = (uint32_t)n;
    memcpy(B->data, a->c, (size_t)a->len * 2);
    memcpy(B->data + a->len, b->c, (size_t)b->len * 2);
    r = js_alloc(rt, sizeof(jstr));
    r->c = B->data;
    r->len = (uint32_t)n;
    r->buf = B;
    return r;
}

jstr *js_substr(js_rt *rt, jstr *s, uint32_t from, uint32_t to)
{
    if (to > s->len)
        to = s->len;
    if (from >= to)
        return rt->empty;
    if (from == 0 && to == s->len)
        return s;
    if (to - from == 1 && s->c[from] < 128)
        return rt->char_str[s->c[from]];
    jstr *r = js_alloc(rt, sizeof(jstr));            /* teksty sa niezmienne - dzielimy znaki */
    r->c = s->c + from;
    r->len = to - from;
    return r;
}

static uint32_t str_hash(jstr *s)
{
    if (!s->hash) {
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < s->len; i++) {
            h ^= s->c[i];
            h *= 16777619u;
        }
        s->hash = h ? h : 1;
    }
    return s->hash;
}

int js_str_eq(const jstr *a, const jstr *b)
{
    if (a == b)
        return 1;
    if (a->len != b->len || (a->atom && b->atom))
        return 0;
    return memcmp(a->c, b->c, (size_t)a->len * 2) == 0;
}

int js_str_cmp(const jstr *a, const jstr *b)
{
    uint32_t n = a->len < b->len ? a->len : b->len;
    for (uint32_t i = 0; i < n; i++)
        if (a->c[i] != b->c[i])
            return a->c[i] < b->c[i] ? -1 : 1;
    return a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
}

int js_str_is(const jstr *s, const char *ascii)
{
    uint32_t i = 0;
    for (; ascii[i]; i++)
        if (i >= s->len || s->c[i] != (uint8_t)ascii[i])
            return 0;
    return i == s->len;
}

jstr *js_intern(js_rt *rt, jstr *s)
{
    if (s->atom)
        return s;
    uint32_t h = str_hash(s) % JS_ATOM_HASH;
    for (jstr *a = rt->atoms[h]; a; a = a->next)
        if (a->len == s->len && memcmp(a->c, s->c, (size_t)s->len * 2) == 0)
            return a;
    if (s->buf) {                                    /* atom nie moze dzielic bufora do dopisywania */
        s = js_str_utf16(rt, s->c, s->len);
        s->hash = 0;
        str_hash(s);
    }
    s->atom = 1;
    s->next = rt->atoms[h];
    rt->atoms[h] = s;
    return s;
}

jstr *js_atom(js_rt *rt, const char *s)
{
    return js_intern(rt, js_str_c(rt, s));
}

unsigned js_utf8(const jstr *s, char *out, unsigned size)
{
    unsigned n = 0;
    if (!size)
        return 0;
    for (uint32_t i = 0; i < s->len; i++) {
        uint32_t cp = s->c[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s->len && s->c[i + 1] >= 0xDC00 && s->c[i + 1] < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s->c[i + 1] - 0xDC00);
            i++;
        } else if (cp >= 0xD800 && cp < 0xE000) {
            cp = 0xFFFD;
        }
        char tmp[4];
        unsigned k = utf8_put(cp, tmp);
        if (n + k >= size)
            break;
        memcpy(out + n, tmp, k);
        n += k;
    }
    out[n] = '\0';
    return n;
}

char *js_utf8_alloc(js_rt *rt, const jstr *s, unsigned *len)
{
    unsigned size = s->len * 3 + 1;
    char *buf = js_alloc(rt, size);
    unsigned n = js_utf8(s, buf, size);
    if (len)
        *len = n;
    return buf;
}

uint16_t js_lower(uint16_t c)
{
    if (c < 128)
        return c >= 'A' && c <= 'Z' ? c + 32 : c;
    if ((c >= 0xC0 && c <= 0xDE && c != 0xD7) || (c >= 0x391 && c <= 0x3AB && c != 0x3A2) || (c >= 0x410 && c <= 0x42F))
        return c + 32;
    if (c >= 0x400 && c <= 0x40F)
        return c + 80;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
        return c | 1;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
        return (c & 1) ? c + 1 : c;
    if (c == 0x178)
        return 0xFF;
    return c;
}

uint16_t js_upper(uint16_t c)
{
    if (c < 128)
        return c >= 'a' && c <= 'z' ? c - 32 : c;
    if ((c >= 0xE0 && c <= 0xFE && c != 0xF7) || (c >= 0x3B1 && c <= 0x3CB && c != 0x3C2) || (c >= 0x430 && c <= 0x44F))
        return c - 32;
    if (c >= 0x450 && c <= 0x45F)
        return c - 80;
    if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177))
        return c & ~1;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E))
        return (c & 1) ? c : c - 1;
    if (c == 0xFF)
        return 0x178;
    return c;
}

int js_is_space(uint16_t c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x0B || c == 0x0C || c == 0xA0 ||
           c == 0xFEFF || c == 0x2028 || c == 0x2029 || (c >= 0x2000 && c <= 0x200A) || c == 0x1680 ||
           c == 0x202F || c == 0x205F || c == 0x3000;
}

/* ---- budowanie tekstu ---- */

void jsb_init(struct jsb *b, js_rt *rt)
{
    b->rt = rt;
    b->c = 0;
    b->len = b->cap = 0;
}

static void jsb_grow(struct jsb *b, uint32_t need)
{
    if (b->len + need <= b->cap)
        return;
    uint64_t cap = b->cap ? (uint64_t)b->cap * 2 : 32;
    while (cap < (uint64_t)b->len + need)
        cap *= 2;
    if (cap > STR_MAX)
        js_throw_error(b->rt, JS_ERR_RANGE, "Invalid string length");
    uint16_t *nc = js_alloc(b->rt, (size_t)cap * 2);
    if (b->len)
        memcpy(nc, b->c, (size_t)b->len * 2);
    b->c = nc;
    b->cap = (uint32_t)cap;
}

void jsb_char(struct jsb *b, uint16_t c)
{
    if (b->len == b->cap)
        jsb_grow(b, 1);
    b->c[b->len++] = c;
}

void jsb_ascii(struct jsb *b, const char *s)
{
    uint32_t n = (uint32_t)strlen(s);
    jsb_grow(b, n);
    for (uint32_t i = 0; i < n; i++)
        b->c[b->len++] = (uint8_t)s[i];
}

void jsb_utf8(struct jsb *b, const char *s, unsigned len)
{
    const char *end = s + len;
    while (s < end) {
        if ((uint8_t)*s < 0x80) {
            jsb_char(b, (uint8_t)*s++);
            continue;
        }
        uint32_t cp = utf8_next(&s, end);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            jsb_char(b, (uint16_t)(0xD800 + (cp >> 10)));
            jsb_char(b, (uint16_t)(0xDC00 + (cp & 0x3FF)));
        } else {
            jsb_char(b, (uint16_t)cp);
        }
    }
}

void jsb_str(struct jsb *b, const jstr *s)
{
    jsb_grow(b, s->len);
    memcpy(b->c + b->len, s->c, (size_t)s->len * 2);
    b->len += s->len;
}

void jsb_num(struct jsb *b, double d)
{
    char buf[40];
    js_dtoa(d, buf);
    jsb_ascii(b, buf);
}

jstr *jsb_done(struct jsb *b)
{
    if (!b->len)
        return b->rt->empty;
    jstr *s = js_alloc(b->rt, sizeof(jstr));
    s->c = b->c;
    s->len = b->len;
    return s;
}

/* ---------- obiekty ---------- */

jobj *js_new_object(js_rt *rt, jobj *proto)
{
    jobj *o = js_alloc(rt, sizeof(jobj));
    o->cls = JC_OBJECT;
    o->proto = proto;
    return o;
}

jobj *js_new_array(js_rt *rt, uint32_t len)
{
    jobj *a = js_new_object(rt, rt->array_proto);
    a->cls = JC_ARRAY;
    if (len) {
        js_array_reserve(rt, a, len);
        for (uint32_t i = 0; i < len; i++)
            a->u.arr.v[i] = jv_undef();
        a->u.arr.len = len;
    }
    return a;
}

#define ARRAY_MAX (1u << 26)

void js_array_reserve(js_rt *rt, jobj *a, uint32_t n)
{
    if (n <= a->u.arr.cap)
        return;
    if (n > ARRAY_MAX)
        js_throw_error(rt, JS_ERR_RANGE, "Invalid array length");
    uint32_t cap = a->u.arr.cap ? a->u.arr.cap * 2 : 8;
    while (cap < n)
        cap *= 2;
    jv *nv = js_alloc(rt, sizeof(jv) * cap);
    if (a->u.arr.len)
        memcpy(nv, a->u.arr.v, sizeof(jv) * a->u.arr.len);
    a->u.arr.v = nv;
    a->u.arr.cap = cap;
}

void js_array_push(js_rt *rt, jobj *a, jv v)
{
    if (a->u.arr.len == a->u.arr.cap)
        js_array_reserve(rt, a, a->u.arr.len + 1);
    a->u.arr.v[a->u.arr.len++] = v;
}

static void array_set_length(js_rt *rt, jobj *a, uint32_t n)
{
    if (n > a->u.arr.len) {
        js_array_reserve(rt, a, n);
        for (uint32_t i = a->u.arr.len; i < n; i++)
            a->u.arr.v[i] = jv_undef();
    }
    a->u.arr.len = n;
}

jobj *js_new_native(js_rt *rt, const char *name, jnative fn, int len)
{
    jobj *f = js_new_object(rt, rt->function_proto);
    f->cls = JC_NATIVE;
    f->u.nf.fn = fn;
    f->u.nf.name = name ? js_atom(rt, name) : rt->empty;
    f->u.nf.len = (int16_t)len;
    return f;
}

static inline uint32_t ptr_hash(const jstr *k)
{
    uint64_t x = (uint64_t)(uintptr_t)k;
    return (uint32_t)((x >> 4) * 2654435761u);
}

static void rebuild_index(js_rt *rt, jobj *o, uint32_t cap)
{
    uint32_t *idx = js_alloc(rt, sizeof(uint32_t) * cap);
    uint32_t mask = cap - 1;
    for (uint32_t i = 0; i < o->nprops; i++) {
        if (!o->props[i].key)
            continue;
        uint32_t h = ptr_hash(o->props[i].key) & mask;
        while (idx[h])
            h = (h + 1) & mask;
        idx[h] = i + 1;
    }
    o->index = idx;
    o->index_cap = cap;
}

struct jprop *js_own(jobj *o, jstr *key)
{
    if (o->index) {
        uint32_t mask = o->index_cap - 1, h = ptr_hash(key) & mask;
        for (;;) {
            uint32_t i = o->index[h];
            if (!i)
                return 0;
            if (o->props[i - 1].key == key)
                return &o->props[i - 1];
            h = (h + 1) & mask;
        }
    }
    struct jprop *p = o->props;
    for (uint32_t i = 0, n = o->nprops; i < n; i++)
        if (p[i].key == key)
            return &p[i];
    return 0;
}

struct jprop *js_find(jobj *o, jstr *key)
{
    for (; o; o = o->proto) {
        struct jprop *p = js_own(o, key);
        if (p)
            return p;
    }
    return 0;
}

static struct jprop *add_prop(js_rt *rt, jobj *o, jstr *key, jv v, uint32_t flags)
{
    if (o->nprops == o->cap) {
        uint32_t cap = o->cap ? o->cap * 2 : 4;
        struct jprop *np = js_alloc(rt, sizeof(struct jprop) * cap);
        if (o->nprops)
            memcpy(np, o->props, sizeof(struct jprop) * o->nprops);
        o->props = np;
        o->cap = cap;
    }
    struct jprop *p = &o->props[o->nprops++];
    p->key = key;
    p->v = v;
    p->flags = flags;
    if (o->index) {
        if (o->nprops * 2 > o->index_cap) {
            rebuild_index(rt, o, o->index_cap * 2);
        } else {
            uint32_t mask = o->index_cap - 1, h = ptr_hash(key) & mask;
            while (o->index[h])
                h = (h + 1) & mask;
            o->index[h] = o->nprops;
        }
    } else if (o->nprops > 8) {
        rebuild_index(rt, o, 32);
    }
    return p;
}

void js_define(js_rt *rt, jobj *o, jstr *key, jv v, uint32_t flags)
{
    struct jprop *p = js_own(o, key);
    if (p) {
        p->v = v;
        p->flags = flags;
        return;
    }
    add_prop(rt, o, key, v, flags);
}

void js_define_acc(js_rt *rt, jobj *o, jstr *key, jobj *get, jobj *set, uint32_t flags)
{
    struct jprop *p = js_own(o, key);
    struct jacc *acc;
    if (p && p->v.t == JT_ACC) {
        acc = p->v.u.acc;
    } else {
        acc = js_alloc(rt, sizeof(*acc));
        jv v;
        v.t = JT_ACC;
        v.u.acc = acc;
        if (p)
            p->v = v;
        else
            p = add_prop(rt, o, key, v, flags);
    }
    if (get)
        acc->get = get;
    if (set)
        acc->set = set;
    p->flags = flags;
}

void js_def_fn(js_rt *rt, jobj *o, const char *name, jnative fn, int len)
{
    js_define(rt, o, js_atom(rt, name), jv_obj(js_new_native(rt, name, fn, len)), PF_WRITE | PF_CONF);
}

void js_def_val(js_rt *rt, jobj *o, const char *name, jv v)
{
    js_define(rt, o, js_atom(rt, name), v, PF_WRITE | PF_CONF);
}

void js_def_getset(js_rt *rt, jobj *o, const char *name, jnative get, jnative set)
{
    char buf[80];
    jobj *g = 0, *s = 0;
    if (get) {
        ksnprintf(buf, sizeof(buf), "get %s", name);
        g = js_new_native(rt, buf, get, 0);
    }
    if (set) {
        ksnprintf(buf, sizeof(buf), "set %s", name);
        s = js_new_native(rt, buf, set, 1);
    }
    js_define_acc(rt, o, js_atom(rt, name), g, s, PF_CONF);
}

int js_array_index(jstr *key, uint32_t *idx)
{
    uint32_t n = key->len;
    if (!n || n > 10)
        return 0;
    const uint16_t *c = key->c;
    if (c[0] < '0' || c[0] > '9' || (c[0] == '0' && n > 1))
        return 0;
    uint64_t v = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (c[i] < '0' || c[i] > '9')
            return 0;
        v = v * 10 + (c[i] - '0');
    }
    if (v >= 0xFFFFFFFFu)
        return 0;
    *idx = (uint32_t)v;
    return 1;
}

jstr *js_func_name(js_rt *rt, jobj *f)
{
    struct jprop *p = js_own(f, rt->A.name);
    if (p && p->v.t == JT_STR)
        return p->v.u.s;
    if (f->cls == JC_NATIVE)
        return f->u.nf.name;
    if (f->cls == JC_FUNC && f->u.f.node && f->u.f.node->u.fn->name)
        return f->u.f.node->u.fn->name;
    return rt->empty;
}

/* f.prototype funkcji tworzymy dopiero, gdy ktos o nie zapyta */
static struct jprop *func_prototype(js_rt *rt, jobj *f)
{
    struct jprop *p = js_own(f, rt->A.prototype);
    if (p || f->cls != JC_FUNC || (f->flags & OF_NOCTOR))
        return p;
    jobj *proto = js_new_object(rt, rt->object_proto);
    js_define(rt, proto, rt->A.constructor, jv_obj(f), PF_WRITE | PF_CONF);
    return add_prop(rt, f, rt->A.prototype, jv_obj(proto), PF_WRITE);
}

static jv call_getter(js_rt *rt, struct jprop *p, jv receiver)
{
    jobj *g = p->v.u.acc->get;
    if (!g)
        return jv_undef();
    return js_call(rt, jv_obj(g), receiver, 0, 0);
}

/* Wlasciwosc obiektu o (z prototypami); receiver = this dla getterow */
static jv get_from(js_rt *rt, jobj *o, jstr *key, jv receiver)
{
    for (; o; o = o->proto) {
        switch (o->cls) {
        case JC_ARRAY:
        case JC_ARGS: {
            if (key == rt->A.length)
                return jv_num(o->u.arr.len);
            uint32_t i;
            if (js_array_index(key, &i))
                return i < o->u.arr.len ? o->u.arr.v[i] : jv_undef();
            break;
        }
        case JC_STR: {
            jstr *s = o->u.prim.u.s;
            if (key == rt->A.length)
                return jv_num(s->len);
            uint32_t i;
            if (js_array_index(key, &i) && i < s->len)
                return jv_str(js_substr(rt, s, i, i + 1));
            break;
        }
        case JC_TYPED: {
            uint32_t i;
            if (js_array_index(key, &i))
                return js_ta_get(o, i);
            break;
        }
        case JC_FUNC:
            if (key == rt->A.prototype) {
                struct jprop *p = func_prototype(rt, o);
                if (p)
                    return p->v;
            }
            /* fallthrough */
        case JC_NATIVE:
        case JC_BOUND:
            if (key == rt->A.name || key == rt->A.length) {
                struct jprop *p = js_own(o, key);
                if (p)
                    return p->v.t == JT_ACC ? call_getter(rt, p, receiver) : p->v;
                if (key == rt->A.name) {
                    if (o->cls == JC_BOUND) {
                        struct jsb b;
                        jsb_init(&b, rt);
                        jsb_ascii(&b, "bound ");
                        jsb_str(&b, js_func_name(rt, o->u.bf.target));
                        return jv_str(jsb_done(&b));
                    }
                    return jv_str(js_func_name(rt, o));
                }
                if (o->cls == JC_NATIVE)
                    return jv_num(o->u.nf.len);
                if (o->cls == JC_BOUND) {
                    jv l = get_from(rt, o->u.bf.target, key, jv_obj(o->u.bf.target));
                    double n = l.t == JT_NUM ? l.u.n - o->u.bf.nargs : 0;
                    return jv_num(n > 0 ? n : 0);
                }
                return jv_num(o->u.f.node ? o->u.f.node->u.fn->nparams : 0);
            }
            break;
        case JC_STYLE:
        case JC_DATASET: {
            jv out;
            if (js_dom_get_hook(rt, o, key, &out))
                return out;
            break;
        }
        }
        struct jprop *p = js_own(o, key);
        if (p)
            return p->v.t == JT_ACC ? call_getter(rt, p, receiver) : p->v;
    }
    return jv_undef();
}

static const char *key_text(js_rt *rt, jstr *key)
{
    unsigned n;
    char *s = js_utf8_alloc(rt, key, &n);
    if (n > 60)
        s[60] = '\0';
    return s;
}

jv js_get(js_rt *rt, jv v, jstr *key)
{
    jobj *o;
    switch (v.t) {
    case JT_OBJ:
        o = v.u.o;
        break;
    case JT_STR: {
        if (key == rt->A.length)
            return jv_num(v.u.s->len);
        uint32_t i;
        if (js_array_index(key, &i))
            return i < v.u.s->len ? jv_str(js_substr(rt, v.u.s, i, i + 1)) : jv_undef();
        o = rt->string_proto;
        break;
    }
    case JT_NUM:
        o = rt->number_proto;
        break;
    case JT_BOOL:
        o = rt->boolean_proto;
        break;
    default:
        js_throw_error(rt, JS_ERR_TYPE, "Cannot read properties of %s (reading '%s')",
                       v.t == JT_NULL ? "null" : "undefined", key_text(rt, key));
    }
    return get_from(rt, o, key, v);
}

jstr *js_key_of(js_rt *rt, jv key)
{
    if (key.t == JT_STR)
        return js_intern(rt, key.u.s);
    if (key.t == JT_NUM) {
        double d = key.u.n;
        if (d >= 0 && d < 256 && d == (double)(int)d)
            return rt->int_str[(int)d];
    }
    return js_intern(rt, js_to_str(rt, key));
}

jv js_get_index(js_rt *rt, jv obj, jv key)
{
    if (key.t == JT_NUM) {
        double d = key.u.n;
        if (obj.t == JT_OBJ && (obj.u.o->cls == JC_ARRAY || obj.u.o->cls == JC_ARGS)) {
            jobj *a = obj.u.o;
            if (d >= 0 && d < a->u.arr.len && d == (double)(uint32_t)d)
                return a->u.arr.v[(uint32_t)d];
        } else if (obj.t == JT_OBJ && obj.u.o->cls == JC_TYPED) {
            if (d >= 0 && d == (double)(uint32_t)d)
                return js_ta_get(obj.u.o, (uint32_t)d);
        } else if (obj.t == JT_STR) {
            if (d >= 0 && d < obj.u.s->len && d == (double)(uint32_t)d)
                return jv_str(js_substr(rt, obj.u.s, (uint32_t)d, (uint32_t)d + 1));
        }
    }
    if (IS_NULLISH(obj)) {
        jstr *k = js_to_str(rt, key);
        js_throw_error(rt, JS_ERR_TYPE, "Cannot read properties of %s (reading '%s')",
                       obj.t == JT_NULL ? "null" : "undefined", key_text(rt, k));
    }
    return js_get(rt, obj, js_key_of(rt, key));
}

void js_set(js_rt *rt, jv target, jstr *key, jv v)
{
    if (target.t != JT_OBJ) {
        if (IS_NULLISH(target))
            js_throw_error(rt, JS_ERR_TYPE, "Cannot set properties of %s (setting '%s')",
                           target.t == JT_NULL ? "null" : "undefined", key_text(rt, key));
        return;                                     /* wlasciwosci liczb i tekstow - ignorujemy */
    }
    jobj *o = target.u.o;
    switch (o->cls) {
    case JC_ARRAY:
    case JC_ARGS: {
        uint32_t i;
        if (key == rt->A.length) {
            double d = js_to_num(rt, v);
            if (d < 0 || d != (double)(uint32_t)d)
                js_throw_error(rt, JS_ERR_RANGE, "Invalid array length");
            if (!(o->flags & OF_FROZEN))
                array_set_length(rt, o, (uint32_t)d);
            return;
        }
        if (js_array_index(key, &i)) {
            if (o->flags & OF_FROZEN)
                return;
            if (i >= o->u.arr.len) {
                if (o->flags & OF_NOEXT)
                    return;
                array_set_length(rt, o, i + 1);
            }
            o->u.arr.v[i] = v;
            return;
        }
        break;
    }
    case JC_STYLE:
    case JC_DATASET:
        if (js_dom_set_hook(rt, o, key, v))
            return;
        break;
    case JC_TYPED: {
        uint32_t i;
        if (js_array_index(key, &i)) {               /* poza zakresem - nic sie nie dzieje */
            js_ta_put(rt, o, i, v);
            return;
        }
        break;
    }
    }
    struct jprop *p = js_own(o, key);
    if (p) {
        if (p->v.t == JT_ACC) {
            if (p->v.u.acc->set)
                js_call(rt, jv_obj(p->v.u.acc->set), target, 1, &v);
            return;
        }
        if (p->flags & PF_WRITE)
            p->v = v;
        return;
    }
    if (o->cls == JC_FUNC && key == rt->A.prototype && !(o->flags & OF_NOCTOR)) {
        p = func_prototype(rt, o);
        if (p) {
            p->v = v;
            return;
        }
    }
    for (jobj *q = o->proto; q; q = q->proto) {
        struct jprop *pp = js_own(q, key);
        if (pp) {
            if (pp->v.t == JT_ACC) {
                if (pp->v.u.acc->set)
                    js_call(rt, jv_obj(pp->v.u.acc->set), target, 1, &v);
                return;
            }
            if (!(pp->flags & PF_WRITE))
                return;
            break;
        }
    }
    if (o->flags & OF_NOEXT)
        return;
    add_prop(rt, o, key, v, PF_ALL);
}

void js_set_index(js_rt *rt, jv obj, jv key, jv v)
{
    if (key.t == JT_NUM && obj.t == JT_OBJ && obj.u.o->cls == JC_TYPED) {
        double d = key.u.n;
        if (d >= 0 && d == (double)(uint32_t)d) {
            js_ta_put(rt, obj.u.o, (uint32_t)d, v);
            return;
        }
    }
    if (key.t == JT_NUM && obj.t == JT_OBJ && obj.u.o->cls == JC_ARRAY && !(obj.u.o->flags & (OF_FROZEN | OF_NOEXT))) {
        jobj *a = obj.u.o;
        double d = key.u.n;
        if (d >= 0 && d == (double)(uint32_t)d && d < ARRAY_MAX) {
            uint32_t i = (uint32_t)d;
            if (i < a->u.arr.len) {
                a->u.arr.v[i] = v;
                return;
            }
            if (i == a->u.arr.len) {
                js_array_push(rt, a, v);
                return;
            }
        }
    }
    if (IS_NULLISH(obj)) {
        jstr *k = js_to_str(rt, key);
        js_throw_error(rt, JS_ERR_TYPE, "Cannot set properties of %s (setting '%s')",
                       obj.t == JT_NULL ? "null" : "undefined", key_text(rt, k));
    }
    js_set(rt, obj, js_key_of(rt, key), v);
}

int js_has_own(js_rt *rt, jobj *o, jstr *key)
{
    uint32_t i;
    switch (o->cls) {
    case JC_ARRAY:
    case JC_ARGS:
        if (key == rt->A.length)
            return 1;
        if (js_array_index(key, &i))
            return i < o->u.arr.len;
        break;
    case JC_STR:
        if (key == rt->A.length)
            return 1;
        if (js_array_index(key, &i))
            return i < o->u.prim.u.s->len;
        break;
    case JC_STYLE:
    case JC_DATASET:
        if (js_dom_has_hook(rt, o, key))
            return 1;
        break;
    case JC_FUNC:
        if (key == rt->A.prototype && func_prototype(rt, o))
            return 1;
        break;
    case JC_TYPED:
        if (js_array_index(key, &i))
            return i < o->u.ta.len;
        break;
    }
    return js_own(o, key) != 0;
}

int js_has(js_rt *rt, jobj *o, jstr *key)
{
    for (; o; o = o->proto)
        if (js_has_own(rt, o, key))
            return 1;
    return 0;
}

int js_delete(js_rt *rt, jobj *o, jstr *key)
{
    if (o->flags & OF_FROZEN)
        return 0;
    if (o->cls == JC_ARRAY || o->cls == JC_ARGS) {
        uint32_t i;
        if (js_array_index(key, &i)) {
            if (i < o->u.arr.len)
                o->u.arr.v[i] = jv_undef();
            return 1;
        }
        if (key == rt->A.length)
            return 0;
    }
    if (o->cls == JC_STYLE || o->cls == JC_DATASET) {
        if (js_dom_has_hook(rt, o, key)) {
            js_dom_set_hook(rt, o, key, jv_undef());
            return 1;
        }
    }
    struct jprop *p = js_own(o, key);
    if (!p)
        return 1;
    if (!(p->flags & PF_CONF))
        return 0;
    p->key = 0;
    p->v = jv_undef();
    return 1;
}

jobj *js_own_keys(js_rt *rt, jobj *o, int only_enum)
{
    jobj *out = js_new_array(rt, 0);
    if (o->cls == JC_ARRAY || o->cls == JC_ARGS) {
        for (uint32_t i = 0; i < o->u.arr.len; i++)
            js_array_push(rt, out, jv_str(js_key_of(rt, jv_num(i))));
    } else if (o->cls == JC_STR) {
        for (uint32_t i = 0; i < o->u.prim.u.s->len; i++)
            js_array_push(rt, out, jv_str(js_key_of(rt, jv_num(i))));
    } else if (o->cls == JC_STYLE || o->cls == JC_DATASET) {
        js_dom_keys_hook(rt, o, out);
    } else if (o->cls == JC_TYPED) {
        for (uint32_t i = 0; i < o->u.ta.len; i++)
            js_array_push(rt, out, jv_str(js_key_of(rt, jv_num(i))));
    }
    if (!only_enum && (o->cls == JC_ARRAY || o->cls == JC_ARGS || o->cls == JC_STR))
        js_array_push(rt, out, jv_str(rt->A.length));
    /* najpierw klucze liczbowe rosnaco (jak w JS), potem reszta w kolejnosci dodania */
    uint32_t first_num = out->u.arr.len;
    for (uint32_t i = 0; i < o->nprops; i++) {
        struct jprop *p = &o->props[i];
        uint32_t idx;
        if (p->key && (!only_enum || (p->flags & PF_ENUM)) && js_array_index(p->key, &idx)) {
            uint32_t j = out->u.arr.len;
            js_array_push(rt, out, jv_str(p->key));
            while (j > first_num) {
                uint32_t other;
                js_array_index(out->u.arr.v[j - 1].u.s, &other);
                if (other <= idx)
                    break;
                out->u.arr.v[j] = out->u.arr.v[j - 1];
                out->u.arr.v[j - 1] = jv_str(p->key);
                j--;
            }
        }
    }
    for (uint32_t i = 0; i < o->nprops; i++) {
        struct jprop *p = &o->props[i];
        uint32_t idx;
        if (p->key && (!only_enum || (p->flags & PF_ENUM)) && !js_array_index(p->key, &idx))
            js_array_push(rt, out, jv_str(p->key));
    }
    return out;
}

/* ---------- zamiany typow ---------- */

int js_truthy(jv v)
{
    switch (v.t) {
    case JT_BOOL: return v.u.b;
    case JT_NUM: return v.u.n != 0 && !js_isnan(v.u.n);
    case JT_STR: return v.u.s->len != 0;
    case JT_OBJ: return 1;
    default: return 0;
    }
}

jv js_to_prim(js_rt *rt, jv v, int hint)
{
    if (v.t != JT_OBJ)
        return v;
    jobj *o = v.u.o;
    if (hint == 0)
        hint = o->cls == JC_DATE ? 2 : 1;
    jstr *order[2] = { rt->A.valueOf, rt->A.toString };
    if (hint == 2) {
        order[0] = rt->A.toString;
        order[1] = rt->A.valueOf;
    }
    for (int i = 0; i < 2; i++) {
        jv f = js_get(rt, v, order[i]);
        if (js_is_callable(f)) {
            jv r = js_call(rt, f, v, 0, 0);
            if (r.t != JT_OBJ)
                return r;
        }
    }
    js_throw_error(rt, JS_ERR_TYPE, "Cannot convert object to primitive value");
}

double js_to_num(js_rt *rt, jv v)
{
    switch (v.t) {
    case JT_NUM: return v.u.n;
    case JT_UNDEF: return js_nan;
    case JT_NULL: return 0;
    case JT_BOOL: return v.u.b;
    case JT_STR: return js_str_to_num(v.u.s);
    default: return js_to_num(rt, js_to_prim(rt, v, 1));
    }
}

jstr *js_to_str(js_rt *rt, jv v)
{
    switch (v.t) {
    case JT_STR: return v.u.s;
    case JT_UNDEF: return rt->A.undefined;
    case JT_NULL: return js_atom(rt, "null");
    case JT_BOOL: return js_atom(rt, v.u.b ? "true" : "false");
    case JT_NUM: {
        double d = v.u.n;
        if (d >= 0 && d < 256 && d == (double)(int)d)
            return rt->int_str[(int)d];
        char buf[40];
        js_dtoa(d, buf);
        return js_str_c(rt, buf);
    }
    default:
        return js_to_str(rt, js_to_prim(rt, v, 2));
    }
}

jobj *js_to_obj(js_rt *rt, jv v)
{
    jobj *o;
    switch (v.t) {
    case JT_OBJ:
        return v.u.o;
    case JT_STR:
        o = js_new_object(rt, rt->string_proto);
        o->cls = JC_STR;
        break;
    case JT_NUM:
        o = js_new_object(rt, rt->number_proto);
        o->cls = JC_NUM;
        break;
    case JT_BOOL:
        o = js_new_object(rt, rt->boolean_proto);
        o->cls = JC_BOOL;
        break;
    default:
        js_throw_error(rt, JS_ERR_TYPE, "Cannot convert undefined or null to object");
    }
    o->u.prim = v;
    return o;
}

static int32_t d2i32(double d)
{
    if (d > -2147483648.0 && d < 2147483648.0)
        return (int32_t)d;
    if (!js_isfinite(d))
        return 0;
    d = js_trunc(d);
    d = js_fmod(d, 4294967296.0);
    if (d < 0)
        d += 4294967296.0;
    return (int32_t)(uint32_t)(uint64_t)d;
}

int32_t js_to_int32(js_rt *rt, jv v)
{
    return d2i32(v.t == JT_NUM ? v.u.n : js_to_num(rt, v));
}

uint32_t js_to_uint32(js_rt *rt, jv v)
{
    return (uint32_t)js_to_int32(rt, v);
}

double js_to_integer(js_rt *rt, jv v)
{
    double d = v.t == JT_NUM ? v.u.n : js_to_num(rt, v);
    if (js_isnan(d))
        return 0;
    return js_trunc(d);
}

int js_strict_eq(jv a, jv b)
{
    if (a.t != b.t)
        return 0;
    switch (a.t) {
    case JT_UNDEF:
    case JT_NULL: return 1;
    case JT_NUM: return a.u.n == b.u.n;
    case JT_BOOL: return a.u.b == b.u.b;
    case JT_STR: return js_str_eq(a.u.s, b.u.s);
    default: return a.u.o == b.u.o;
    }
}

int js_same_value_zero(jv a, jv b)
{
    if (a.t == JT_NUM && b.t == JT_NUM && js_isnan(a.u.n) && js_isnan(b.u.n))
        return 1;
    return js_strict_eq(a, b);
}

int js_loose_eq(js_rt *rt, jv a, jv b)
{
    for (;;) {
        if (a.t == b.t)
            return js_strict_eq(a, b);
        if (IS_NULLISH(a) && IS_NULLISH(b))
            return 1;
        if (IS_NULLISH(a) || IS_NULLISH(b))
            return 0;
        if (a.t == JT_NUM && b.t == JT_STR)
            return a.u.n == js_str_to_num(b.u.s);
        if (a.t == JT_STR && b.t == JT_NUM)
            return js_str_to_num(a.u.s) == b.u.n;
        if (a.t == JT_BOOL) {
            a = jv_num(a.u.b);
            continue;
        }
        if (b.t == JT_BOOL) {
            b = jv_num(b.u.b);
            continue;
        }
        if (a.t == JT_OBJ && b.t != JT_OBJ) {
            a = js_to_prim(rt, a, 0);
            continue;
        }
        if (b.t == JT_OBJ && a.t != JT_OBJ) {
            b = js_to_prim(rt, b, 0);
            continue;
        }
        return 0;
    }
}

int js_is_callable(jv v)
{
    if (v.t != JT_OBJ)
        return 0;
    int c = v.u.o->cls;
    return c == JC_FUNC || c == JC_NATIVE || c == JC_BOUND;
}

int js_is_ctor(jv v)
{
    if (v.t != JT_OBJ)
        return 0;
    jobj *o = v.u.o;
    switch (o->cls) {
    case JC_FUNC: return !(o->flags & OF_NOCTOR);
    case JC_NATIVE: return o->u.nf.ctor != 0;
    case JC_BOUND: return js_is_ctor(jv_obj(o->u.bf.target));
    }
    return 0;
}

const char *js_typeof(jv v)
{
    switch (v.t) {
    case JT_UNDEF: return "undefined";
    case JT_NULL: return "object";
    case JT_BOOL: return "boolean";
    case JT_NUM: return "number";
    case JT_STR: return "string";
    default: return js_is_callable(v) ? "function" : "object";
    }
}

/* ---------- bledy ---------- */

static const char *const error_names[] = {
    "Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError", "EvalError", "URIError",
};

jobj *js_make_error(js_rt *rt, int kind, jstr *msg)
{
    jobj *e = js_new_object(rt, rt->error_protos[kind]);
    e->cls = JC_ERROR;
    if (msg)
        js_define(rt, e, rt->A.message, jv_str(msg), PF_WRITE | PF_CONF);
    char buf[160];
    char m[100];
    if (msg)
        js_utf8(msg, m, sizeof(m));
    else
        m[0] = '\0';
    ksnprintf(buf, sizeof(buf), "%s: %s\n    at %s:%u", error_names[kind], m,
              rt->script_name ? rt->script_name : "skrypt", rt->line);
    js_define(rt, e, rt->A.stack, jv_str(js_str_c(rt, buf)), PF_WRITE | PF_CONF);
    return e;
}

void js_throw(js_rt *rt, jv v)
{
    rt->exc = v;
    rt->exc_line = rt->line;
    js_longjmp(&rt->catch_top->jb, 1);
}

void js_throw_error(js_rt *rt, int kind, const char *fmt, ...)
{
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    js_throw(rt, jv_obj(js_make_error(rt, kind, js_str_c(rt, buf))));
}

void js_abort(js_rt *rt, const char *msg)
{
    rt->aborting = 1;
    rt->abort_msg = msg;
    rt->exc = jv_undef();
    rt->exc_line = rt->line;
    js_longjmp(&rt->catch_top->jb, 1);
}

void js_check_time(js_rt *rt)
{
    rt->next_check = rt->steps + 8192;
    if (rt->cancel && *rt->cancel)
        js_abort(rt, "Skrypt przerwany");
    uint32_t now = timer_uptime_ms();
    if (now - rt->last_yield >= 30) {
        thread_yield();                             /* pulpit i siec tez chca dzialac */
        rt->last_yield = timer_uptime_ms();
    }
    if (rt->budget_ms && now - rt->run_start > rt->budget_ms)
        js_abort(rt, "Skrypt dzialal za dlugo - przerwany (petla bez konca?)");
}

void js_check_stack(js_rt *rt)
{
    if (rt->on_stack && (uintptr_t)__builtin_frame_address(0) < rt->stack_limit)
        js_throw_error(rt, JS_ERR_RANGE, "Maximum call stack size exceeded");
}

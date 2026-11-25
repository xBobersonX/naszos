/* Tablice typowane: ArrayBuffer, Int8Array ... Float64Array, DataView,
 * TextEncoder i TextDecoder.
 *
 * ArrayBuffer to po prostu kawalek bajtow. Tablica typowana (np. Uint16Array)
 * to "okno" na taki kawalek: element i to bajty [off + i*rozmiar, ...),
 * czytane jako liczba danego typu. Kilka tablic moze patrzec na ten sam
 * bufor (subarray, new Uint8Array(buf, 4, 8)) - zmiany widac wszedzie.
 *
 * Indeksy (t[i]) obsluguje js_core.c (js_ta_get / js_ta_put), a metody
 * w rodzaju forEach, map, join wolaja zwykle metody tablic na kopii. */

#include "js_int.h"
#include "string.h"

#define U __attribute__((unused))
#define NATIVE(name) static jv name(js_rt *rt U, jv self U, int argc U, jv *argv U, jobj *fn U)
#define ARG(i) ((i) < argc ? argv[i] : jv_undef())

#define BUF_MAX (256u << 20)             /* najwiekszy ArrayBuffer */

enum { TA_I8, TA_U8, TA_U8C, TA_I16, TA_U16, TA_I32, TA_U32, TA_F32, TA_F64, TA_COUNT };
static const char *const ta_names[TA_COUNT] = {
    "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array",
    "Int32Array", "Uint32Array", "Float32Array", "Float64Array",
};
static const uint8_t ta_size[TA_COUNT] = { 1, 1, 1, 2, 2, 4, 4, 4, 8 };

/* ---------- liczby ---------- */

static uint32_t d2u(double d)                    /* ToUint32: modulo 2^32 */
{
    if (!js_isfinite(d))
        return 0;
    d = d < 0 ? -js_floor(-d) : js_floor(d);
    d = js_fmod(d, 4294967296.0);
    if (d < 0)
        d += 4294967296.0;
    return (uint32_t)(uint64_t)d;
}

static uint8_t clamp8(double d)                  /* Uint8ClampedArray: 0..255, polowki do parzystej */
{
    if (!(d > 0))
        return 0;
    if (d >= 255)
        return 255;
    double f = js_floor(d), r = d - f;
    if (r > 0.5 || (r == 0.5 && ((int)f & 1)))
        f += 1;
    return (uint8_t)f;
}

static double get_raw(const uint8_t *p, int kind)
{
    switch (kind) {
    case TA_I8: return (int8_t)p[0];
    case TA_U8: case TA_U8C: return p[0];
    case TA_I16: { int16_t v; memcpy(&v, p, 2); return v; }
    case TA_U16: { uint16_t v; memcpy(&v, p, 2); return v; }
    case TA_I32: { int32_t v; memcpy(&v, p, 4); return v; }
    case TA_U32: { uint32_t v; memcpy(&v, p, 4); return v; }
    case TA_F32: { float v; memcpy(&v, p, 4); return v; }
    default: { double v; memcpy(&v, p, 8); return v; }
    }
}

static void put_raw(uint8_t *p, int kind, double d)
{
    switch (kind) {
    case TA_I8: case TA_U8: p[0] = (uint8_t)d2u(d); break;
    case TA_U8C: p[0] = clamp8(d); break;
    case TA_I16: case TA_U16: { uint16_t v = (uint16_t)d2u(d); memcpy(p, &v, 2); break; }
    case TA_I32: case TA_U32: { uint32_t v = d2u(d); memcpy(p, &v, 4); break; }
    case TA_F32: { float v = (float)d; memcpy(p, &v, 4); break; }
    default: memcpy(p, &d, 8); break;
    }
}

static uint8_t *ta_ptr(jobj *t, uint32_t i)
{
    return t->u.ta.buf->u.ab.data + t->u.ta.off + (size_t)i * ta_size[t->u.ta.kind];
}

jv js_ta_get(jobj *t, uint32_t i)
{
    return i < t->u.ta.len ? jv_num(get_raw(ta_ptr(t, i), t->u.ta.kind)) : jv_undef();
}

void js_ta_put(js_rt *rt, jobj *t, uint32_t i, jv v)
{
    double d = v.t == JT_NUM ? v.u.n : js_to_num(rt, v);
    if (i < t->u.ta.len)
        put_raw(ta_ptr(t, i), t->u.ta.kind, d);
}

/* ---------- tworzenie ---------- */

static uint32_t to_index(js_rt *rt, jv v, const char *what)
{
    if (v.t == JT_UNDEF)
        return 0;
    double d = js_to_num(rt, v);
    if (d != d)
        return 0;
    if (d < 0 || d > BUF_MAX)
        js_throw_error(rt, JS_ERR_RANGE, "Invalid %s", what);
    return (uint32_t)d;
}

static jobj *new_buf(js_rt *rt, uint32_t len)
{
    if (len > BUF_MAX)
        js_throw_error(rt, JS_ERR_RANGE, "Array buffer allocation failed");
    jobj *b = js_new_object(rt, rt->abuf_proto);
    b->cls = JC_ABUF;
    b->u.ab.data = js_alloc_raw(rt, len ? len : 1);
    memset(b->u.ab.data, 0, len ? len : 1);
    b->u.ab.len = len;
    return b;
}

static jobj *new_view(js_rt *rt, int kind, jobj *buf, uint32_t off, uint32_t len)
{
    jobj *t = js_new_object(rt, rt->ta_protos[kind]);
    t->cls = JC_TYPED;
    t->u.ta.buf = buf;
    t->u.ta.off = off;
    t->u.ta.len = len;
    t->u.ta.kind = (uint8_t)kind;
    return t;
}

static jobj *new_ta(js_rt *rt, int kind, uint32_t len)
{
    if ((uint64_t)len * ta_size[kind] > BUF_MAX)
        js_throw_error(rt, JS_ERR_RANGE, "Invalid typed array length: %u", len);
    return new_view(rt, kind, new_buf(rt, len * ta_size[kind]), 0, len);
}

static jobj *this_ta(js_rt *rt, jv self)
{
    if (self.t != JT_OBJ || self.u.o->cls != JC_TYPED)
        js_throw_error(rt, JS_ERR_TYPE, "this is not a typed array.");
    return self.u.o;
}

/* Kopia elementow jako zwykla tablica JS */
jv js_ta_to_array(js_rt *rt, jobj *t)
{
    jobj *a = js_new_array(rt, t->u.ta.len);
    for (uint32_t i = 0; i < t->u.ta.len; i++)
        a->u.arr.v[i] = js_ta_get(t, i);
    return jv_obj(a);
}

/* Zrodlo -> tablica JS: tablica typowana, iterowalne albo "array-like" ({length: 3, 0: ...}) */
static jobj *items_of(js_rt *rt, jv src)
{
    if (src.t == JT_OBJ && src.u.o->cls == JC_TYPED)
        return js_ta_to_array(rt, src.u.o).u.o;
    jv itf = js_get(rt, src, rt->sym_iterator);
    if (js_is_callable(itf) || src.t == JT_STR)
        return js_iterate_to_array(rt, src).u.o;
    uint32_t n = d2u(js_to_num(rt, js_get(rt, src, rt->A.length)));
    jobj *a = js_new_array(rt, n);
    for (uint32_t i = 0; i < n; i++)
        a->u.arr.v[i] = js_get_index(rt, src, jv_num(i));
    return a;
}

static jobj *from_items(js_rt *rt, int kind, jobj *items)
{
    jobj *t = new_ta(rt, kind, items->u.arr.len);
    for (uint32_t i = 0; i < items->u.arr.len; i++)
        js_ta_put(rt, t, i, items->u.arr.v[i]);
    return t;
}

static int kind_of(jobj *fn) { return (int)fn->u.nf.data.u.n; }

static jv ta_ctor(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn)
{
    int kind = kind_of(fn);
    jv a = ARG(0);
    if (a.t != JT_OBJ)
        return jv_obj(new_ta(rt, kind, to_index(rt, a, "typed array length")));
    if (a.u.o->cls == JC_ABUF) {                         /* widok na istniejacy bufor */
        jobj *b = a.u.o;
        uint32_t sz = ta_size[kind], off = to_index(rt, ARG(1), "offset"), len;
        if (off % sz)
            js_throw_error(rt, JS_ERR_RANGE, "start offset of %s should be a multiple of %u", ta_names[kind], sz);
        if (ARG(2).t == JT_UNDEF) {
            if (off > b->u.ab.len || (b->u.ab.len - off) % sz)
                js_throw_error(rt, JS_ERR_RANGE, "byte length of %s should be a multiple of %u", ta_names[kind], sz);
            len = (b->u.ab.len - off) / sz;
        } else {
            len = to_index(rt, ARG(2), "typed array length");
            if ((uint64_t)off + (uint64_t)len * sz > b->u.ab.len)
                js_throw_error(rt, JS_ERR_RANGE, "Invalid typed array length: %u", len);
        }
        return jv_obj(new_view(rt, kind, b, off, len));
    }
    return jv_obj(from_items(rt, kind, items_of(rt, a)));
}

static jv need_new(js_rt *rt, jv self U, int argc U, jv *argv U, jobj *fn)
{
    char name[40];
    js_utf8(js_func_name(rt, fn), name, sizeof(name));
    js_throw_error(rt, JS_ERR_TYPE, "Constructor %s requires 'new'", name);
}

/* Uint8Array.from(src, mapFn), Uint8Array.of(...) - "this" to konstruktor */
static int ctor_kind(js_rt *rt, jv self)
{
    if (self.t == JT_OBJ && self.u.o->cls == JC_NATIVE && self.u.o->u.nf.ctor == ta_ctor)
        return kind_of(self.u.o);
    js_throw_error(rt, JS_ERR_TYPE, "this is not a typed array constructor");
}

NATIVE(ta_from)
{
    int kind = ctor_kind(rt, self);
    jobj *items = items_of(rt, ARG(0));
    if (js_is_callable(ARG(1)))
        for (uint32_t i = 0; i < items->u.arr.len; i++) {
            jv args[2] = { items->u.arr.v[i], jv_num(i) };
            items->u.arr.v[i] = js_call(rt, ARG(1), ARG(2), 2, args);
        }
    return jv_obj(from_items(rt, kind, items));
}

NATIVE(ta_of)
{
    int kind = ctor_kind(rt, self);
    jobj *t = new_ta(rt, kind, (uint32_t)argc);
    for (int i = 0; i < argc; i++)
        js_ta_put(rt, t, (uint32_t)i, argv[i]);
    return jv_obj(t);
}

/* ---------- wlasnosci i metody tablic typowanych ---------- */

NATIVE(ta_length) { return jv_num(this_ta(rt, self)->u.ta.len); }
NATIVE(ta_byte_length) { jobj *t = this_ta(rt, self); return jv_num((double)t->u.ta.len * ta_size[t->u.ta.kind]); }
NATIVE(ta_byte_offset) { return jv_num(this_ta(rt, self)->u.ta.off); }
NATIVE(ta_buffer) { return jv_obj(this_ta(rt, self)->u.ta.buf); }

/* Indeks wzgledny (ujemny = od konca), przyciety do 0..len */
static uint32_t rel(js_rt *rt, jv v, uint32_t len, uint32_t def)
{
    if (v.t == JT_UNDEF)
        return def;
    double d = js_to_num(rt, v);
    if (d != d)
        return 0;
    d = d < 0 ? -js_floor(-d) : js_floor(d);
    if (d < 0)
        d += len;
    return d < 0 ? 0 : d > len ? len : (uint32_t)d;
}

NATIVE(ta_subarray)
{
    jobj *t = this_ta(rt, self);
    uint32_t b = rel(rt, ARG(0), t->u.ta.len, 0), e = rel(rt, ARG(1), t->u.ta.len, t->u.ta.len);
    if (e < b)
        e = b;
    return jv_obj(new_view(rt, t->u.ta.kind, t->u.ta.buf, t->u.ta.off + b * ta_size[t->u.ta.kind], e - b));
}

NATIVE(ta_set)
{
    jobj *t = this_ta(rt, self);
    uint32_t off = to_index(rt, ARG(1), "offset");
    jv src = ARG(0);
    if (src.t == JT_OBJ && src.u.o->cls == JC_TYPED) {
        jobj *s = src.u.o;
        if ((uint64_t)off + s->u.ta.len > t->u.ta.len)
            js_throw_error(rt, JS_ERR_RANGE, "offset is out of bounds");
        if (s->u.ta.kind == t->u.ta.kind) {              /* ten sam typ: kopia bajtow (memmove - moga sie nakladac) */
            uint8_t *d = ta_ptr(t, off), *p = ta_ptr(s, 0);
            size_t n = (size_t)s->u.ta.len * ta_size[s->u.ta.kind];
            if (d < p)
                for (size_t i = 0; i < n; i++) d[i] = p[i];
            else
                for (size_t i = n; i-- > 0;) d[i] = p[i];
            return jv_undef();
        }
    }
    jobj *items = items_of(rt, src);
    if ((uint64_t)off + items->u.arr.len > t->u.ta.len)
        js_throw_error(rt, JS_ERR_RANGE, "offset is out of bounds");
    for (uint32_t i = 0; i < items->u.arr.len; i++)
        js_ta_put(rt, t, off + i, items->u.arr.v[i]);
    return jv_undef();
}

static void sort_nums(double *v, uint32_t n)             /* Shell sort; NaN na koniec */
{
    static const uint32_t gaps[] = { 701, 301, 132, 57, 23, 10, 4, 1 };
    for (int g = 0; g < 8; g++)
        for (uint32_t i = gaps[g]; i < n; i++) {
            double x = v[i];
            uint32_t j = i;
            while (j >= gaps[g] && (v[j - gaps[g]] > x || (v[j - gaps[g]] != v[j - gaps[g]] && x == x))) {
                v[j] = v[j - gaps[g]];
                j -= gaps[g];
            }
            v[j] = x;
        }
}

/* Metody "jak w tablicy": robimy kopie jako Array, wolamy metode Array.prototype.
 * data = nazwa; flags w nazwie: tryb 0 wynik bez zmian, 1 wynik -> nowa tablica
 * typowana, 2 zmiany wracaja do tej tablicy (zwracamy this) */
static jv via_array(js_rt *rt, jv self, int argc, jv *argv, jobj *fn)
{
    jobj *t = this_ta(rt, self);
    jstr *name = fn->u.nf.data.u.s;
    int mode = name->c[name->len - 1] == '!' ? 2 : name->c[name->len - 1] == '+' ? 1 : 0;
    jstr *method = mode ? js_intern(rt, js_substr(rt, name, 0, name->len - 1)) : name;
    if (mode == 2 && method == js_atom(rt, "sort") && ARG(0).t == JT_UNDEF) {   /* domyslnie liczbowo */
        uint32_t n = t->u.ta.len;
        double *v = js_alloc_raw(rt, sizeof(double) * (n ? n : 1));
        for (uint32_t i = 0; i < n; i++)
            v[i] = get_raw(ta_ptr(t, i), t->u.ta.kind);
        sort_nums(v, n);
        for (uint32_t i = 0; i < n; i++)
            put_raw(ta_ptr(t, i), t->u.ta.kind, v[i]);
        return self;
    }
    jv arr = js_ta_to_array(rt, t);
    jv m = js_get(rt, jv_obj(rt->array_proto), method);
    jv r = js_call(rt, m, arr, argc, argv);
    if (mode == 1)
        return jv_obj(from_items(rt, t->u.ta.kind, r.u.o));
    if (mode == 2) {
        for (uint32_t i = 0; i < t->u.ta.len; i++)
            js_ta_put(rt, t, i, arr.u.o->u.arr.v[i]);
        return self;
    }
    return r;
}

NATIVE(ta_values) { return jv_obj(js_make_iter(rt, js_ta_to_array(rt, this_ta(rt, self)).u.o, IT_VALUES)); }
NATIVE(ta_keys) { return jv_obj(js_make_iter(rt, js_ta_to_array(rt, this_ta(rt, self)).u.o, IT_KEYS)); }
NATIVE(ta_entries) { return jv_obj(js_make_iter(rt, js_ta_to_array(rt, this_ta(rt, self)).u.o, IT_ENTRIES)); }

/* ---------- ArrayBuffer ---------- */

static jobj *this_buf(js_rt *rt, jv self)
{
    if (self.t != JT_OBJ || self.u.o->cls != JC_ABUF)
        js_throw_error(rt, JS_ERR_TYPE, "this is not an ArrayBuffer");
    return self.u.o;
}

static jv ab_ctor(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn U)
{
    return jv_obj(new_buf(rt, to_index(rt, ARG(0), "array buffer length")));
}

NATIVE(ab_byte_length) { return jv_num(this_buf(rt, self)->u.ab.len); }

NATIVE(ab_slice)
{
    jobj *b = this_buf(rt, self);
    uint32_t s = rel(rt, ARG(0), b->u.ab.len, 0), e = rel(rt, ARG(1), b->u.ab.len, b->u.ab.len);
    jobj *n = new_buf(rt, e > s ? e - s : 0);
    if (e > s)
        memcpy(n->u.ab.data, b->u.ab.data + s, e - s);
    return jv_obj(n);
}

NATIVE(ab_is_view)
{
    jv v = ARG(0);
    return jv_bool(v.t == JT_OBJ && (v.u.o->cls == JC_TYPED || v.u.o->cls == JC_DVIEW));
}

/* ---------- DataView ---------- */

static jobj *this_dv(js_rt *rt, jv self)
{
    if (self.t != JT_OBJ || self.u.o->cls != JC_DVIEW)
        js_throw_error(rt, JS_ERR_TYPE, "this is not a DataView");
    return self.u.o;
}

static jv dv_ctor(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn U)
{
    jv a = ARG(0);
    if (a.t != JT_OBJ || a.u.o->cls != JC_ABUF)
        js_throw_error(rt, JS_ERR_TYPE, "First argument to DataView constructor must be an ArrayBuffer");
    jobj *b = a.u.o;
    uint32_t off = to_index(rt, ARG(1), "DataView offset");
    if (off > b->u.ab.len)
        js_throw_error(rt, JS_ERR_RANGE, "Start offset %u is outside the bounds of the buffer", off);
    uint32_t len = ARG(2).t == JT_UNDEF ? b->u.ab.len - off : to_index(rt, ARG(2), "DataView length");
    if ((uint64_t)off + len > b->u.ab.len)
        js_throw_error(rt, JS_ERR_RANGE, "Invalid DataView length %u", len);
    jobj *d = js_new_object(rt, rt->dview_proto);
    d->cls = JC_DVIEW;
    d->u.ta.buf = b;
    d->u.ta.off = off;
    d->u.ta.len = len;
    return jv_obj(d);
}

NATIVE(dv_buffer) { return jv_obj(this_dv(rt, self)->u.ta.buf); }
NATIVE(dv_byte_length) { return jv_num(this_dv(rt, self)->u.ta.len); }
NATIVE(dv_byte_offset) { return jv_num(this_dv(rt, self)->u.ta.off); }

/* getInt16(off, littleEndian) / setInt16(off, v, littleEndian); data = rodzaj */
static uint8_t *dv_at(js_rt *rt, jobj *d, jv off, int size)
{
    uint32_t o = to_index(rt, off, "offset");
    if ((uint64_t)o + size > d->u.ta.len)
        js_throw_error(rt, JS_ERR_RANGE, "Offset is outside the bounds of the DataView");
    return d->u.ta.buf->u.ab.data + d->u.ta.off + o;
}

static void swap(uint8_t *p, int n)
{
    for (int i = 0; i < n / 2; i++) {
        uint8_t t = p[i];
        p[i] = p[n - 1 - i];
        p[n - 1 - i] = t;
    }
}

static jv dv_get(js_rt *rt, jv self, int argc, jv *argv, jobj *fn)
{
    int kind = kind_of(fn), n = ta_size[kind];
    uint8_t tmp[8], *p = dv_at(rt, this_dv(rt, self), ARG(0), n);
    memcpy(tmp, p, (size_t)n);
    if (!js_truthy(ARG(1)))                              /* domyslnie big-endian */
        swap(tmp, n);
    return jv_num(get_raw(tmp, kind));
}

static jv dv_set(js_rt *rt, jv self, int argc, jv *argv, jobj *fn)
{
    int kind = kind_of(fn), n = ta_size[kind];
    uint8_t tmp[8], *p = dv_at(rt, this_dv(rt, self), ARG(0), n);
    put_raw(tmp, kind, js_to_num(rt, ARG(1)));
    if (!js_truthy(ARG(2)))
        swap(tmp, n);
    memcpy(p, tmp, (size_t)n);
    return jv_undef();
}

/* ---------- TextEncoder / TextDecoder (UTF-8) ---------- */

static jv te_ctor(js_rt *rt, jv self U, int argc U, jv *argv U, jobj *fn)
{
    return jv_obj(js_new_object(rt, js_get(rt, jv_obj(fn), rt->A.prototype).u.o));
}

NATIVE(te_encode)
{
    unsigned n;
    char *s = js_utf8_alloc(rt, js_to_str(rt, ARG(0).t == JT_UNDEF ? jv_str(rt->empty) : ARG(0)), &n);
    jobj *t = new_ta(rt, TA_U8, n);
    memcpy(t->u.ta.buf->u.ab.data, s, n);
    return jv_obj(t);
}

NATIVE(te_encoding) { return jv_str(js_atom(rt, "utf-8")); }

NATIVE(td_decode)
{
    jv a = ARG(0);
    const uint8_t *p = 0;
    uint32_t n = 0;
    if (a.t == JT_OBJ && a.u.o->cls == JC_ABUF) {
        p = a.u.o->u.ab.data;
        n = a.u.o->u.ab.len;
    } else if (a.t == JT_OBJ && (a.u.o->cls == JC_TYPED || a.u.o->cls == JC_DVIEW)) {
        p = a.u.o->u.ta.buf->u.ab.data + a.u.o->u.ta.off;
        n = a.u.o->cls == JC_DVIEW ? a.u.o->u.ta.len : a.u.o->u.ta.len * ta_size[a.u.o->u.ta.kind];
    }
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) {  /* BOM */
        p += 3;
        n -= 3;
    }
    return jv_str(js_str_utf8(rt, (const char *)p, n));
}

/* ---------- rejestracja ---------- */

static jobj *ctor(js_rt *rt, const char *name, jnative call, jnative make, int len, jobj *proto, jv data)
{
    jobj *f = js_new_native(rt, name, call, len);
    f->u.nf.ctor = make;
    f->u.nf.data = data;
    js_define(rt, f, rt->A.prototype, jv_obj(proto), 0);
    js_define(rt, proto, rt->A.constructor, jv_obj(f), PF_WRITE | PF_CONF);
    js_def_val(rt, rt->global, name, jv_obj(f));
    return f;
}

static void def_named(js_rt *rt, jobj *o, const char *name, jnative fn, int len, const char *data)
{
    js_def_fn(rt, o, name, fn, len);
    jobj *f = js_get(rt, jv_obj(o), js_atom(rt, name)).u.o;
    f->u.nf.data = jv_str(js_atom(rt, data));
}

void js_typed_init(js_rt *rt)
{
    /* ArrayBuffer */
    rt->abuf_proto = js_new_object(rt, rt->object_proto);
    jobj *ab = ctor(rt, "ArrayBuffer", need_new, ab_ctor, 1, rt->abuf_proto, jv_undef());
    js_def_getset(rt, rt->abuf_proto, "byteLength", ab_byte_length, 0);
    js_def_fn(rt, rt->abuf_proto, "slice", ab_slice, 2);
    js_def_fn(rt, ab, "isView", ab_is_view, 1);

    /* wspolny prototyp wszystkich tablic typowanych (%TypedArray%.prototype) */
    jobj *tp = js_new_object(rt, rt->object_proto);
    js_def_getset(rt, tp, "length", ta_length, 0);
    js_def_getset(rt, tp, "byteLength", ta_byte_length, 0);
    js_def_getset(rt, tp, "byteOffset", ta_byte_offset, 0);
    js_def_getset(rt, tp, "buffer", ta_buffer, 0);
    js_def_fn(rt, tp, "subarray", ta_subarray, 2);
    js_def_fn(rt, tp, "set", ta_set, 1);
    js_def_fn(rt, tp, "values", ta_values, 0);
    js_def_fn(rt, tp, "keys", ta_keys, 0);
    js_def_fn(rt, tp, "entries", ta_entries, 0);
    js_define(rt, tp, rt->sym_iterator, js_get(rt, jv_obj(tp), js_atom(rt, "values")), PF_WRITE | PF_CONF);
    static const char *const plain[] = {
        "forEach", "join", "indexOf", "lastIndexOf", "includes", "every", "some", "find", "findIndex",
        "findLast", "findLastIndex", "reduce", "reduceRight", "at", "toString", "toLocaleString",
    };
    for (unsigned i = 0; i < sizeof(plain) / sizeof(plain[0]); i++)
        def_named(rt, tp, plain[i], via_array, 1, plain[i]);
    def_named(rt, tp, "map", via_array, 1, "map+");
    def_named(rt, tp, "filter", via_array, 1, "filter+");
    def_named(rt, tp, "slice", via_array, 2, "slice+");
    def_named(rt, tp, "fill", via_array, 1, "fill!");
    def_named(rt, tp, "reverse", via_array, 0, "reverse!");
    def_named(rt, tp, "sort", via_array, 1, "sort!");
    def_named(rt, tp, "copyWithin", via_array, 2, "copyWithin!");

    for (int k = 0; k < TA_COUNT; k++) {
        jobj *p = js_new_object(rt, tp);
        rt->ta_protos[k] = p;
        jobj *f = ctor(rt, ta_names[k], need_new, ta_ctor, 3, p, jv_num(k));
        js_def_val(rt, f, "BYTES_PER_ELEMENT", jv_num(ta_size[k]));
        js_def_val(rt, p, "BYTES_PER_ELEMENT", jv_num(ta_size[k]));
        js_def_fn(rt, f, "from", ta_from, 1);
        js_def_fn(rt, f, "of", ta_of, 0);
    }

    /* DataView */
    rt->dview_proto = js_new_object(rt, rt->object_proto);
    ctor(rt, "DataView", need_new, dv_ctor, 1, rt->dview_proto, jv_undef());
    js_def_getset(rt, rt->dview_proto, "buffer", dv_buffer, 0);
    js_def_getset(rt, rt->dview_proto, "byteLength", dv_byte_length, 0);
    js_def_getset(rt, rt->dview_proto, "byteOffset", dv_byte_offset, 0);
    static const char *const dvn[TA_COUNT] = { "Int8", "Uint8", 0, "Int16", "Uint16", "Int32", "Uint32",
                                               "Float32", "Float64" };
    for (int k = 0; k < TA_COUNT; k++) {
        if (!dvn[k])
            continue;
        char name[24];
        ksnprintf(name, sizeof(name), "get%s", dvn[k]);
        js_def_fn(rt, rt->dview_proto, name, dv_get, 1);
        js_get(rt, jv_obj(rt->dview_proto), js_atom(rt, name)).u.o->u.nf.data = jv_num(k);
        ksnprintf(name, sizeof(name), "set%s", dvn[k]);
        js_def_fn(rt, rt->dview_proto, name, dv_set, 2);
        js_get(rt, jv_obj(rt->dview_proto), js_atom(rt, name)).u.o->u.nf.data = jv_num(k);
    }

    /* TextEncoder / TextDecoder */
    jobj *tep = js_new_object(rt, rt->object_proto);
    ctor(rt, "TextEncoder", need_new, te_ctor, 0, tep, jv_undef());
    js_def_fn(rt, tep, "encode", te_encode, 1);
    js_def_getset(rt, tep, "encoding", te_encoding, 0);
    jobj *tdp = js_new_object(rt, rt->object_proto);
    ctor(rt, "TextDecoder", need_new, te_ctor, 0, tdp, jv_undef());
    js_def_fn(rt, tdp, "decode", td_decode, 1);
    js_def_getset(rt, tdp, "encoding", te_encoding, 0);
}

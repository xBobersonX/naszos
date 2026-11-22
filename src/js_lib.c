/* Obiekty wbudowane JavaScriptu (czesc 1): globalne funkcje, Object,
 * Function, Array, Error, Boolean, Number, Math, JSON, console.
 * Czesc 2 (String, Date, Map/Set, Promise) - js_lib2.c.
 *
 * Kazda funkcja wbudowana ma postac:
 *     jv nazwa(rt, this, liczba_argumentow, argumenty, sama_funkcja)
 * i jest wpisywana do swojego obiektu (np. Array.prototype.push). */

#include "js_int.h"
#include "string.h"
#include "timer.h"
#include "rtc.h"

#define U __attribute__((unused))
#define NATIVE(name) static jv name(js_rt *rt U, jv self U, int argc U, jv *argv U, jobj *fn U)
#define ARG(i) ((i) < argc ? argv[i] : jv_undef())

void js_lib2_init(js_rt *rt);

static jstr *S(js_rt *rt, const char *s) { return js_str_c(rt, s); }

static void type_error(js_rt *rt, const char *msg) __attribute__((noreturn));
static void type_error(js_rt *rt, const char *msg)
{
    js_throw_error(rt, JS_ERR_TYPE, "%s", msg);
}

static jobj *this_obj(js_rt *rt, jv self)
{
    if (IS_NULLISH(self))
        type_error(rt, "Cannot convert undefined or null to object");
    return js_to_obj(rt, self);
}

static jobj *callback(js_rt *rt, jv f)
{
    if (!js_is_callable(f)) {
        jstr *s = js_to_str(rt, f.t == JT_OBJ ? jv_str(js_atom(rt, "object")) : f);
        char buf[80];
        js_utf8(s, buf, sizeof(buf));
        js_throw_error(rt, JS_ERR_TYPE, "%s is not a function", buf);
    }
    return f.u.o;
}

/* Tablica z "this": prawdziwa tablica albo cos podobnego (arguments, {length: 2, 0: .., 1: ..}) */
static jobj *array_like(js_rt *rt, jv self)
{
    if (self.t == JT_OBJ && (self.u.o->cls == JC_ARRAY || self.u.o->cls == JC_ARGS))
        return self.u.o;
    jobj *o = this_obj(rt, self);
    jobj *a = js_new_array(rt, 0);
    double n = js_to_integer(rt, js_get(rt, jv_obj(o), rt->A.length));
    if (n > (1 << 26))
        js_throw_error(rt, JS_ERR_RANGE, "Invalid array length");
    for (uint32_t i = 0; i < (uint32_t)(n > 0 ? n : 0); i++)
        js_array_push(rt, a, js_get_index(rt, jv_obj(o), jv_num(i)));
    return a;
}

static jobj *this_array(js_rt *rt, jv self)
{
    if (self.t == JT_OBJ && (self.u.o->cls == JC_ARRAY || self.u.o->cls == JC_ARGS))
        return self.u.o;
    type_error(rt, "Array.prototype method called on non-array");
}

/* Indeks wzgledny: -1 = ostatni; przyciety do [0, len] */
static uint32_t rel_index(js_rt *rt, jv v, uint32_t len, uint32_t def)
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

/* ---------- funkcje globalne ---------- */

NATIVE(g_parse_int)
{
    jstr *s = js_to_str(rt, ARG(0));
    int radix = argc > 1 ? js_to_int32(rt, argv[1]) : 0;
    uint32_t i = 0;
    while (i < s->len && js_is_space(s->c[i]))
        i++;
    int neg = 0;
    if (i < s->len && (s->c[i] == '+' || s->c[i] == '-'))
        neg = s->c[i++] == '-';
    if (radix == 0) {
        radix = 10;
        if (i + 1 < s->len && s->c[i] == '0' && (s->c[i + 1] | 0x20) == 'x') {
            radix = 16;
            i += 2;
        }
    } else if (radix == 16 && i + 1 < s->len && s->c[i] == '0' && (s->c[i + 1] | 0x20) == 'x') {
        i += 2;
    }
    if (radix < 2 || radix > 36)
        return jv_num(js_nan);
    double v = 0;
    int any = 0;
    if (radix == 10) {
        char buf[400];
        unsigned n = 0;
        while (i < s->len && s->c[i] >= '0' && s->c[i] <= '9' && n < sizeof(buf) - 1)
            buf[n++] = (char)s->c[i++];
        buf[n] = '\0';
        if (!n)
            return jv_num(js_nan);
        v = js_strtod(buf, 0);
        return jv_num(neg ? -v : v);
    }
    for (; i < s->len; i++) {
        uint16_t c = s->c[i];
        int d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'z' ? (c | 0x20) - 'a' + 10 : 99;
        if (d >= radix)
            break;
        v = v * radix + d;
        any = 1;
    }
    if (!any)
        return jv_num(js_nan);
    return jv_num(neg ? -v : v);
}

NATIVE(g_parse_float)
{
    jstr *s = js_to_str(rt, ARG(0));
    uint32_t i = 0;
    while (i < s->len && js_is_space(s->c[i]))
        i++;
    char buf[400];
    unsigned n = 0;
    while (i < s->len && n < sizeof(buf) - 1 && s->c[i] < 128)
        buf[n++] = (char)s->c[i++];
    buf[n] = '\0';
    const char *p = buf;
    int neg = 0;
    if (*p == '+' || *p == '-')
        neg = *p++ == '-';
    if (memcmp(p, "Infinity", 8) == 0)
        return jv_num(neg ? -js_inf : js_inf);
    const char *end;
    double v = js_strtod(buf, &end);
    if (!end)
        return jv_num(js_nan);
    return jv_num(v);
}

NATIVE(g_is_nan) { return jv_bool(js_isnan(js_to_num(rt, ARG(0)))); }
NATIVE(g_is_finite) { return jv_bool(js_isfinite(js_to_num(rt, ARG(0)))); }

static int uri_unreserved(uint16_t c, const char *extra)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return 1;
    for (const char *e = "-_.!~*'()"; *e; e++)
        if (c == (uint8_t)*e)
            return 1;
    for (; extra && *extra; extra++)
        if (c == (uint8_t)*extra)
            return 1;
    return 0;
}

static jv uri_encode(js_rt *rt, jv v, const char *extra)
{
    jstr *s = js_to_str(rt, v);
    struct jsb b;
    jsb_init(&b, rt);
    static const char hex[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < s->len; i++) {
        uint32_t c = s->c[i];
        if (c < 128 && uri_unreserved((uint16_t)c, extra)) {
            jsb_char(&b, (uint16_t)c);
            continue;
        }
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s->len) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s->c[++i] - 0xDC00);
        } else if (c >= 0xD800 && c < 0xE000) {
            js_throw_error(rt, JS_ERR_URI, "URI malformed");
        }
        char u[4];
        unsigned n = 0;
        if (c < 0x80) u[n++] = (char)c;
        else if (c < 0x800) { u[n++] = (char)(0xC0 | c >> 6); u[n++] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { u[n++] = (char)(0xE0 | c >> 12); u[n++] = (char)(0x80 | (c >> 6 & 0x3F)); u[n++] = (char)(0x80 | (c & 0x3F)); }
        else { u[n++] = (char)(0xF0 | c >> 18); u[n++] = (char)(0x80 | (c >> 12 & 0x3F)); u[n++] = (char)(0x80 | (c >> 6 & 0x3F)); u[n++] = (char)(0x80 | (c & 0x3F)); }
        for (unsigned k = 0; k < n; k++) {
            jsb_char(&b, '%');
            jsb_char(&b, (uint16_t)hex[(uint8_t)u[k] >> 4]);
            jsb_char(&b, (uint16_t)hex[(uint8_t)u[k] & 15]);
        }
    }
    return jv_str(jsb_done(&b));
}

static int hexd(uint16_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') return (c | 0x20) - 'a' + 10;
    return -1;
}

static jv uri_decode(js_rt *rt, jv v, const char *keep)
{
    jstr *s = js_to_str(rt, v);
    char *bytes = js_alloc(rt, s->len * 3 + 1);
    unsigned n = 0;
    for (uint32_t i = 0; i < s->len; i++) {
        uint16_t c = s->c[i];
        if (c == '%' && i + 2 < s->len && hexd(s->c[i + 1]) >= 0 && hexd(s->c[i + 2]) >= 0) {
            int byte = hexd(s->c[i + 1]) * 16 + hexd(s->c[i + 2]);
            int kept = 0;
            for (const char *k = keep; k && *k; k++)
                if (byte == (uint8_t)*k)
                    kept = 1;
            if (kept) {
                bytes[n++] = '%';
                continue;
            }
            bytes[n++] = (char)byte;
            i += 2;
        } else if (c == '%') {
            js_throw_error(rt, JS_ERR_URI, "URI malformed");
        } else {
            char tmp[4];
            jstr one = { &s->c[i], 1, 0, 0, 0, 0, 0 };
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < s->len) {
                one.len = 2;
                i++;
            }
            unsigned k = js_utf8(&one, tmp, sizeof(tmp));
            memcpy(bytes + n, tmp, k);
            n += k;
        }
    }
    return jv_str(js_str_utf8(rt, bytes, n));
}

NATIVE(g_encode_uri_component) { return uri_encode(rt, ARG(0), 0); }
NATIVE(g_encode_uri) { return uri_encode(rt, ARG(0), ";/?:@&=+$,#"); }
NATIVE(g_decode_uri_component) { return uri_decode(rt, ARG(0), 0); }
NATIVE(g_decode_uri) { return uri_decode(rt, ARG(0), ";/?:@&=+$,#"); }

NATIVE(g_escape)
{
    jstr *s = js_to_str(rt, ARG(0));
    struct jsb b;
    jsb_init(&b, rt);
    static const char hex[] = "0123456789ABCDEF";
    for (uint32_t i = 0; i < s->len; i++) {
        uint16_t c = s->c[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '@' || c == '*' || c == '_' || c == '+' || c == '-' || c == '.' || c == '/') {
            jsb_char(&b, c);
        } else if (c < 256) {
            jsb_char(&b, '%');
            jsb_char(&b, (uint16_t)hex[c >> 4]);
            jsb_char(&b, (uint16_t)hex[c & 15]);
        } else {
            jsb_ascii(&b, "%u");
            for (int k = 12; k >= 0; k -= 4)
                jsb_char(&b, (uint16_t)hex[(c >> k) & 15]);
        }
    }
    return jv_str(jsb_done(&b));
}

NATIVE(g_unescape)
{
    jstr *s = js_to_str(rt, ARG(0));
    struct jsb b;
    jsb_init(&b, rt);
    for (uint32_t i = 0; i < s->len; i++) {
        uint16_t c = s->c[i];
        if (c == '%' && i + 5 < s->len && s->c[i + 1] == 'u' && hexd(s->c[i + 2]) >= 0 && hexd(s->c[i + 3]) >= 0 &&
            hexd(s->c[i + 4]) >= 0 && hexd(s->c[i + 5]) >= 0) {
            jsb_char(&b, (uint16_t)(hexd(s->c[i + 2]) << 12 | hexd(s->c[i + 3]) << 8 | hexd(s->c[i + 4]) << 4 | hexd(s->c[i + 5])));
            i += 5;
        } else if (c == '%' && i + 2 < s->len && hexd(s->c[i + 1]) >= 0 && hexd(s->c[i + 2]) >= 0) {
            jsb_char(&b, (uint16_t)(hexd(s->c[i + 1]) * 16 + hexd(s->c[i + 2])));
            i += 2;
        } else {
            jsb_char(&b, c);
        }
    }
    return jv_str(jsb_done(&b));
}

NATIVE(g_eval)
{
    jv code = ARG(0);
    if (code.t != JT_STR)
        return code;
    unsigned len;
    char *src = js_utf8_alloc(rt, code.u.s, &len);
    return js_run_program(rt, src, len);
}

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

NATIVE(g_btoa)
{
    jstr *s = js_to_str(rt, ARG(0));
    struct jsb b;
    jsb_init(&b, rt);
    for (uint32_t i = 0; i < s->len; i++)
        if (s->c[i] > 255)
            js_throw_error(rt, JS_ERR_ERROR, "Failed to execute 'btoa': The string to be encoded contains characters outside of the Latin1 range.");
    for (uint32_t i = 0; i < s->len; i += 3) {
        uint32_t v = (uint32_t)s->c[i] << 16 | (i + 1 < s->len ? (uint32_t)s->c[i + 1] << 8 : 0) | (i + 2 < s->len ? s->c[i + 2] : 0);
        jsb_char(&b, (uint16_t)b64[v >> 18 & 63]);
        jsb_char(&b, (uint16_t)b64[v >> 12 & 63]);
        jsb_char(&b, i + 1 < s->len ? (uint16_t)b64[v >> 6 & 63] : '=');
        jsb_char(&b, i + 2 < s->len ? (uint16_t)b64[v & 63] : '=');
    }
    return jv_str(jsb_done(&b));
}

NATIVE(g_atob)
{
    jstr *s = js_to_str(rt, ARG(0));
    struct jsb b;
    jsb_init(&b, rt);
    uint32_t acc = 0;
    int bits = 0;
    for (uint32_t i = 0; i < s->len; i++) {
        uint16_t c = s->c[i];
        int v = -1;
        for (int k = 0; k < 64; k++)
            if (b64[k] == c)
                v = k;
        if (c == '=' || js_is_space(c))
            continue;
        if (v < 0)
            js_throw_error(rt, JS_ERR_ERROR, "Failed to execute 'atob': The string to be decoded is not correctly encoded.");
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            jsb_char(&b, (uint16_t)(acc >> bits & 0xFF));
        }
    }
    return jv_str(jsb_done(&b));
}

/* ---------- Object ---------- */

static jv object_ctor(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn U)
{
    jv v = ARG(0);
    if (IS_NULLISH(v))
        return jv_obj(js_new_object(rt, rt->object_proto));
    return jv_obj(js_to_obj(rt, v));
}

NATIVE(obj_keys) { return jv_obj(js_own_keys(rt, this_obj(rt, ARG(0)), 1)); }

NATIVE(obj_values)
{
    jobj *o = this_obj(rt, ARG(0)), *keys = js_own_keys(rt, o, 1), *out = js_new_array(rt, 0);
    for (uint32_t i = 0; i < keys->u.arr.len; i++)
        js_array_push(rt, out, js_get(rt, jv_obj(o), keys->u.arr.v[i].u.s));
    return jv_obj(out);
}

NATIVE(obj_entries)
{
    jobj *o = this_obj(rt, ARG(0)), *keys = js_own_keys(rt, o, 1), *out = js_new_array(rt, 0);
    for (uint32_t i = 0; i < keys->u.arr.len; i++) {
        jobj *pair = js_new_array(rt, 0);
        js_array_push(rt, pair, keys->u.arr.v[i]);
        js_array_push(rt, pair, js_get(rt, jv_obj(o), keys->u.arr.v[i].u.s));
        js_array_push(rt, out, jv_obj(pair));
    }
    return jv_obj(out);
}

NATIVE(obj_assign)
{
    jobj *t = this_obj(rt, ARG(0));
    for (int i = 1; i < argc; i++) {
        if (IS_NULLISH(argv[i]))
            continue;
        jobj *src = js_to_obj(rt, argv[i]), *keys = js_own_keys(rt, src, 1);
        for (uint32_t k = 0; k < keys->u.arr.len; k++) {
            jstr *key = keys->u.arr.v[k].u.s;
            js_set(rt, jv_obj(t), key, js_get(rt, jv_obj(src), key));
        }
    }
    return jv_obj(t);
}

static void define_from_descriptor(js_rt *rt, jobj *o, jstr *key, jv desc)
{
    if (desc.t != JT_OBJ)
        type_error(rt, "Property description must be an object");
    jv get = js_get(rt, desc, rt->A.get), set = js_get(rt, desc, rt->A.set);
    uint32_t flags = 0;
    struct jprop *old = js_own(o, key);
    if (old)
        flags = old->flags;
    jv e = js_get(rt, desc, rt->A.enumerable), c = js_get(rt, desc, rt->A.configurable),
       w = js_get(rt, desc, rt->A.writable);
    if (e.t != JT_UNDEF) flags = js_truthy(e) ? flags | PF_ENUM : flags & ~(uint32_t)PF_ENUM;
    if (c.t != JT_UNDEF) flags = js_truthy(c) ? flags | PF_CONF : flags & ~(uint32_t)PF_CONF;
    if (get.t != JT_UNDEF || set.t != JT_UNDEF) {
        js_define_acc(rt, o, key, js_is_callable(get) ? get.u.o : 0, js_is_callable(set) ? set.u.o : 0, flags & ~(uint32_t)PF_WRITE);
        return;
    }
    if (w.t != JT_UNDEF) flags = js_truthy(w) ? flags | PF_WRITE : flags & ~(uint32_t)PF_WRITE;
    jv v = js_get(rt, desc, rt->A.value);
    if (o->cls == JC_ARRAY) {
        uint32_t idx;
        if (js_array_index(key, &idx) || key == rt->A.length) {
            js_set(rt, jv_obj(o), key, v);
            return;
        }
    }
    if (!js_has_own(rt, o, rt->A.value) && old && v.t == JT_UNDEF && !js_has_own(rt, desc.u.o, rt->A.value))
        v = old->v.t == JT_ACC ? jv_undef() : old->v;
    js_define(rt, o, key, v, flags);
}

NATIVE(obj_define_property)
{
    jv o = ARG(0);
    if (o.t != JT_OBJ)
        type_error(rt, "Object.defineProperty called on non-object");
    define_from_descriptor(rt, o.u.o, js_key_of(rt, ARG(1)), ARG(2));
    return o;
}

NATIVE(obj_define_properties)
{
    jv o = ARG(0);
    if (o.t != JT_OBJ)
        type_error(rt, "Object.defineProperties called on non-object");
    jobj *props = this_obj(rt, ARG(1)), *keys = js_own_keys(rt, props, 1);
    for (uint32_t i = 0; i < keys->u.arr.len; i++)
        define_from_descriptor(rt, o.u.o, keys->u.arr.v[i].u.s, js_get(rt, jv_obj(props), keys->u.arr.v[i].u.s));
    return o;
}

NATIVE(obj_create)
{
    jv p = ARG(0);
    if (p.t != JT_OBJ && p.t != JT_NULL)
        type_error(rt, "Object prototype may only be an Object or null");
    jobj *o = js_new_object(rt, p.t == JT_OBJ ? p.u.o : 0);
    if (argc > 1 && argv[1].t == JT_OBJ) {
        jobj *keys = js_own_keys(rt, argv[1].u.o, 1);
        for (uint32_t i = 0; i < keys->u.arr.len; i++)
            define_from_descriptor(rt, o, keys->u.arr.v[i].u.s, js_get(rt, argv[1], keys->u.arr.v[i].u.s));
    }
    return jv_obj(o);
}

NATIVE(obj_get_own_property_names) { return jv_obj(js_own_keys(rt, this_obj(rt, ARG(0)), 0)); }

NATIVE(obj_get_own_property_descriptor)
{
    jobj *o = this_obj(rt, ARG(0));
    jstr *key = js_key_of(rt, ARG(1));
    if (!js_has_own(rt, o, key))
        return jv_undef();
    jobj *d = js_new_object(rt, rt->object_proto);
    struct jprop *p = js_own(o, key);
    if (p && p->v.t == JT_ACC) {
        js_set(rt, jv_obj(d), rt->A.get, p->v.u.acc->get ? jv_obj(p->v.u.acc->get) : jv_undef());
        js_set(rt, jv_obj(d), rt->A.set, p->v.u.acc->set ? jv_obj(p->v.u.acc->set) : jv_undef());
    } else {
        js_set(rt, jv_obj(d), rt->A.value, js_get(rt, jv_obj(o), key));
        js_set(rt, jv_obj(d), rt->A.writable, jv_bool(!p || (p->flags & PF_WRITE)));
    }
    js_set(rt, jv_obj(d), rt->A.enumerable, jv_bool(!p || (p->flags & PF_ENUM)));
    js_set(rt, jv_obj(d), rt->A.configurable, jv_bool(!p || (p->flags & PF_CONF)));
    return jv_obj(d);
}

NATIVE(obj_get_prototype_of)
{
    jobj *o = this_obj(rt, ARG(0));
    return o->proto ? jv_obj(o->proto) : jv_null();
}

NATIVE(obj_set_prototype_of)
{
    jv o = ARG(0), p = ARG(1);
    if (o.t == JT_OBJ && (p.t == JT_OBJ || p.t == JT_NULL)) {
        for (jobj *q = p.t == JT_OBJ ? p.u.o : 0; q; q = q->proto)
            if (q == o.u.o)
                type_error(rt, "Cyclic __proto__ value");
        o.u.o->proto = p.t == JT_OBJ ? p.u.o : 0;
    }
    return o;
}

NATIVE(obj_freeze)
{
    jv o = ARG(0);
    if (o.t == JT_OBJ) {
        o.u.o->flags |= OF_FROZEN | OF_NOEXT;
        for (uint32_t i = 0; i < o.u.o->nprops; i++)
            o.u.o->props[i].flags &= ~(uint32_t)(PF_WRITE | PF_CONF);
    }
    return o;
}

NATIVE(obj_seal)
{
    jv o = ARG(0);
    if (o.t == JT_OBJ) {
        o.u.o->flags |= OF_NOEXT;
        for (uint32_t i = 0; i < o.u.o->nprops; i++)
            o.u.o->props[i].flags &= ~(uint32_t)PF_CONF;
    }
    return o;
}

NATIVE(obj_prevent_extensions)
{
    if (ARG(0).t == JT_OBJ)
        argv[0].u.o->flags |= OF_NOEXT;
    return ARG(0);
}

NATIVE(obj_is_frozen) { return jv_bool(ARG(0).t != JT_OBJ || (argv[0].u.o->flags & OF_FROZEN)); }
NATIVE(obj_is_sealed) { return jv_bool(ARG(0).t != JT_OBJ || (argv[0].u.o->flags & OF_NOEXT)); }
NATIVE(obj_is_extensible) { return jv_bool(ARG(0).t == JT_OBJ && !(argv[0].u.o->flags & OF_NOEXT)); }

NATIVE(obj_from_entries)
{
    jobj *o = js_new_object(rt, rt->object_proto);
    jobj *list = js_iterate_to_array(rt, ARG(0)).u.o;
    for (uint32_t i = 0; i < list->u.arr.len; i++) {
        jv e = list->u.arr.v[i];
        js_set(rt, jv_obj(o), js_key_of(rt, js_get_index(rt, e, jv_num(0))), js_get_index(rt, e, jv_num(1)));
    }
    return jv_obj(o);
}

NATIVE(obj_is)
{
    jv a = ARG(0), b = ARG(1);
    if (a.t == JT_NUM && b.t == JT_NUM) {
        if (js_isnan(a.u.n) && js_isnan(b.u.n))
            return jv_bool(1);
        if (a.u.n == 0 && b.u.n == 0)
            return jv_bool((1 / a.u.n > 0) == (1 / b.u.n > 0));
    }
    return jv_bool(js_strict_eq(a, b));
}

NATIVE(objp_has_own)
{
    jobj *o = this_obj(rt, self);
    return jv_bool(js_has_own(rt, o, js_key_of(rt, ARG(0))));
}

NATIVE(obj_has_own)
{
    jobj *o = this_obj(rt, ARG(0));
    return jv_bool(js_has_own(rt, o, js_key_of(rt, ARG(1))));
}

NATIVE(objp_is_prototype_of)
{
    jv v = ARG(0);
    if (v.t != JT_OBJ || self.t != JT_OBJ)
        return jv_bool(0);
    for (jobj *o = v.u.o->proto; o; o = o->proto)
        if (o == self.u.o)
            return jv_bool(1);
    return jv_bool(0);
}

NATIVE(objp_property_is_enumerable)
{
    jobj *o = this_obj(rt, self);
    jstr *key = js_key_of(rt, ARG(0));
    struct jprop *p = js_own(o, key);
    if (p)
        return jv_bool(p->flags & PF_ENUM);
    return jv_bool(js_has_own(rt, o, key) && key != rt->A.length);
}

static const char *class_name(js_rt *rt, jv v)
{
    if (v.t == JT_UNDEF) return "Undefined";
    if (v.t == JT_NULL) return "Null";
    if (v.t == JT_NUM) return "Number";
    if (v.t == JT_STR) return "String";
    if (v.t == JT_BOOL) return "Boolean";
    switch (v.u.o->cls) {
    case JC_ARRAY: return "Array";
    case JC_FUNC: case JC_NATIVE: case JC_BOUND: return "Function";
    case JC_ERROR: return "Error";
    case JC_BOOL: return "Boolean";
    case JC_NUM: return "Number";
    case JC_STR: return "String";
    case JC_DATE: return "Date";
    case JC_REGEXP: return "RegExp";
    case JC_ARGS: return "Arguments";
    case JC_MAP: return "Map";
    case JC_SET: return "Set";
    case JC_PROMISE: return "Promise";
    }
    (void)rt;
    return "Object";
}

NATIVE(objp_to_string)
{
    char buf[40];
    ksnprintf(buf, sizeof(buf), "[object %s]", class_name(rt, self));
    return jv_str(S(rt, buf));
}

NATIVE(objp_to_locale_string)
{
    jv f = js_get(rt, self, rt->A.toString);
    return js_call(rt, f, self, 0, 0);
}

NATIVE(objp_value_of) { return jv_obj(this_obj(rt, self)); }

NATIVE(objp_proto_get)
{
    jobj *o = this_obj(rt, self);
    return o->proto ? jv_obj(o->proto) : jv_null();
}

NATIVE(objp_proto_set)
{
    jv p = ARG(0);
    if (self.t == JT_OBJ && (p.t == JT_OBJ || p.t == JT_NULL))
        self.u.o->proto = p.t == JT_OBJ ? p.u.o : 0;
    return jv_undef();
}

/* ---------- Function ---------- */

static jv function_ctor(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn U)
{
    struct jsb params;
    jsb_init(&params, rt);
    for (int i = 0; i < argc - 1; i++) {
        if (i)
            jsb_char(&params, ',');
        jsb_str(&params, js_to_str(rt, argv[i]));
    }
    jstr *body = argc ? js_to_str(rt, argv[argc - 1]) : rt->empty;
    char *p8 = js_utf8_alloc(rt, jsb_done(&params), 0), *b8 = js_utf8_alloc(rt, body, 0);
    return jv_obj(js_function_from_text(rt, p8, b8));
}

NATIVE(fn_call)
{
    return js_call(rt, self, ARG(0), argc > 1 ? argc - 1 : 0, argv + 1);
}

NATIVE(fn_apply)
{
    jv list = ARG(1);
    if (IS_NULLISH(list))
        return js_call(rt, self, ARG(0), 0, 0);
    if (list.t != JT_OBJ)
        type_error(rt, "CreateListFromArrayLike called on non-object");
    jobj *a = array_like(rt, list);
    return js_call(rt, self, ARG(0), (int)a->u.arr.len, a->u.arr.v);
}

NATIVE(fn_bind)
{
    if (!js_is_callable(self))
        type_error(rt, "Bind must be called on a function");
    jobj *b = js_new_object(rt, self.u.o->proto);
    b->cls = JC_BOUND;
    b->u.bf.target = self.u.o;
    b->u.bf.self = ARG(0);
    if (argc > 1) {
        b->u.bf.nargs = argc - 1;
        b->u.bf.args = js_alloc(rt, sizeof(jv) * (size_t)(argc - 1));
        memcpy(b->u.bf.args, argv + 1, sizeof(jv) * (size_t)(argc - 1));
    }
    return jv_obj(b);
}

NATIVE(fn_to_string)
{
    if (!js_is_callable(self))
        type_error(rt, "Function.prototype.toString requires that 'this' be a Function");
    jobj *f = self.u.o;
    if (f->cls == JC_FUNC && f->u.f.node && f->u.f.node->u.fn->src)
        return jv_str(js_str_utf8(rt, f->u.f.node->u.fn->src, f->u.f.node->u.fn->src_len));
    struct jsb b;
    jsb_init(&b, rt);
    jsb_ascii(&b, f->flags & OF_CLASS ? "class " : "function ");
    jsb_str(&b, js_func_name(rt, f));
    jsb_ascii(&b, f->flags & OF_CLASS ? " { }" : "() { [native code] }");
    return jv_str(jsb_done(&b));
}

/* ---------- Array ---------- */

static jv array_ctor(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn U)
{
    if (argc == 1 && argv[0].t == JT_NUM) {
        double n = argv[0].u.n;
        if (n < 0 || n != (double)(uint32_t)n)
            js_throw_error(rt, JS_ERR_RANGE, "Invalid array length");
        return jv_obj(js_new_array(rt, (uint32_t)n));
    }
    jobj *a = js_new_array(rt, 0);
    for (int i = 0; i < argc; i++)
        js_array_push(rt, a, argv[i]);
    return jv_obj(a);
}

NATIVE(arr_is_array) { return jv_bool(ARG(0).t == JT_OBJ && argv[0].u.o->cls == JC_ARRAY); }

NATIVE(arr_of)
{
    jobj *a = js_new_array(rt, 0);
    for (int i = 0; i < argc; i++)
        js_array_push(rt, a, argv[i]);
    return jv_obj(a);
}

NATIVE(arr_from)
{
    jv src = ARG(0), map = ARG(1);
    jobj *out = js_new_array(rt, 0), *items;
    if (IS_NULLISH(src))
        type_error(rt, "Array.from requires an array-like object");
    if (src.t == JT_STR || (src.t == JT_OBJ && (src.u.o->cls == JC_MAP || src.u.o->cls == JC_SET ||
                                               src.u.o->cls == JC_ARRAY || js_has(rt, src.u.o, rt->sym_iterator))))
        items = js_iterate_to_array(rt, src).u.o;
    else
        items = array_like(rt, src);
    int has_map = js_is_callable(map);
    for (uint32_t i = 0; i < items->u.arr.len; i++) {
        jv v = items->u.arr.v[i];
        if (has_map) {
            jv a[2] = { v, jv_num(i) };
            v = js_call(rt, map, ARG(2), 2, a);
        }
        js_array_push(rt, out, v);
    }
    return jv_obj(out);
}

NATIVE(arr_push)
{
    if (self.t == JT_OBJ && self.u.o->cls == JC_ARRAY && !(self.u.o->flags & OF_NOEXT)) {
        jobj *a = self.u.o;
        js_array_reserve(rt, a, a->u.arr.len + (uint32_t)argc);
        for (int i = 0; i < argc; i++)
            a->u.arr.v[a->u.arr.len++] = argv[i];
        return jv_num(a->u.arr.len);
    }
    jobj *o = this_obj(rt, self);
    double n = js_to_integer(rt, js_get(rt, jv_obj(o), rt->A.length));
    for (int i = 0; i < argc; i++)
        js_set_index(rt, jv_obj(o), jv_num(n++), argv[i]);
    js_set(rt, jv_obj(o), rt->A.length, jv_num(n));
    return jv_num(n);
}

NATIVE(arr_pop)
{
    jobj *a = this_array(rt, self);
    if (!a->u.arr.len || (a->flags & OF_FROZEN))
        return jv_undef();
    return a->u.arr.v[--a->u.arr.len];
}

NATIVE(arr_shift)
{
    jobj *a = this_array(rt, self);
    if (!a->u.arr.len || (a->flags & OF_FROZEN))
        return jv_undef();
    jv v = a->u.arr.v[0];
    memmove(a->u.arr.v, a->u.arr.v + 1, sizeof(jv) * (a->u.arr.len - 1));
    a->u.arr.len--;
    return v;
}

NATIVE(arr_unshift)
{
    jobj *a = this_array(rt, self);
    js_array_reserve(rt, a, a->u.arr.len + (uint32_t)argc);
    memmove(a->u.arr.v + argc, a->u.arr.v, sizeof(jv) * a->u.arr.len);
    for (int i = 0; i < argc; i++)
        a->u.arr.v[i] = argv[i];
    a->u.arr.len += (uint32_t)argc;
    return jv_num(a->u.arr.len);
}

NATIVE(arr_slice)
{
    jobj *a = array_like(rt, self);
    uint32_t len = a->u.arr.len;
    uint32_t from = rel_index(rt, ARG(0), len, 0), to = rel_index(rt, ARG(1), len, len);
    jobj *out = js_new_array(rt, 0);
    if (to > from) {
        js_array_reserve(rt, out, to - from);
        memcpy(out->u.arr.v, a->u.arr.v + from, sizeof(jv) * (to - from));
        out->u.arr.len = to - from;
    }
    return jv_obj(out);
}

NATIVE(arr_splice)
{
    jobj *a = this_array(rt, self);
    uint32_t len = a->u.arr.len;
    uint32_t start = rel_index(rt, ARG(0), len, 0);
    uint32_t del = argc == 0 ? 0 : argc == 1 ? len - start : 0;
    if (argc >= 2) {
        double d = js_to_integer(rt, argv[1]);
        if (d < 0) d = 0;
        if (d > len - start) d = len - start;
        del = (uint32_t)d;
    }
    jobj *removed = js_new_array(rt, del);
    if (del)
        memcpy(removed->u.arr.v, a->u.arr.v + start, sizeof(jv) * del);
    uint32_t ins = argc > 2 ? (uint32_t)(argc - 2) : 0;
    uint32_t newlen = len - del + ins;
    js_array_reserve(rt, a, newlen);
    memmove(a->u.arr.v + start + ins, a->u.arr.v + start + del, sizeof(jv) * (len - start - del));
    for (uint32_t i = 0; i < ins; i++)
        a->u.arr.v[start + i] = argv[2 + i];
    a->u.arr.len = newlen;
    return jv_obj(removed);
}

NATIVE(arr_concat)
{
    jobj *out = js_new_array(rt, 0);
    for (int i = -1; i < argc; i++) {
        jv v = i < 0 ? self : argv[i];
        if (v.t == JT_OBJ && v.u.o->cls == JC_ARRAY) {
            jobj *src = v.u.o;
            js_array_reserve(rt, out, out->u.arr.len + src->u.arr.len);
            memcpy(out->u.arr.v + out->u.arr.len, src->u.arr.v, sizeof(jv) * src->u.arr.len);
            out->u.arr.len += src->u.arr.len;
        } else {
            js_array_push(rt, out, v);
        }
    }
    return jv_obj(out);
}

jv js_array_join(js_rt *rt, jobj *a, jstr *sep)
{
    for (int i = 0; i < rt->join_depth; i++)
        if (rt->join_stack[i] == a)
            return jv_str(rt->empty);                /* a = [a] - bez nieskonczonosci */
    if (rt->join_depth >= 32)
        return jv_str(rt->empty);
    rt->join_stack[rt->join_depth++] = a;
    struct jsb b;
    jsb_init(&b, rt);
    for (uint32_t i = 0; i < a->u.arr.len; i++) {
        if (i)
            jsb_str(&b, sep);
        jv v = a->u.arr.v[i];
        if (!IS_NULLISH(v))
            jsb_str(&b, js_to_str(rt, v));
    }
    rt->join_depth--;
    return jv_str(jsb_done(&b));
}

NATIVE(arr_join)
{
    jobj *a = array_like(rt, self);
    jstr *sep = ARG(0).t == JT_UNDEF ? js_atom(rt, ",") : js_to_str(rt, argv[0]);
    return js_array_join(rt, a, sep);
}

NATIVE(arr_to_string)
{
    if (self.t != JT_OBJ || self.u.o->cls != JC_ARRAY)
        return objp_to_string(rt, self, argc, argv, fn);
    return js_array_join(rt, self.u.o, js_atom(rt, ","));
}

NATIVE(arr_reverse)
{
    jobj *a = this_array(rt, self);
    for (uint32_t i = 0, j = a->u.arr.len; i + 1 < j; i++, j--) {
        jv t = a->u.arr.v[i];
        a->u.arr.v[i] = a->u.arr.v[j - 1];
        a->u.arr.v[j - 1] = t;
    }
    return self;
}

NATIVE(arr_index_of)
{
    jobj *a = array_like(rt, self);
    uint32_t len = a->u.arr.len;
    for (uint32_t i = rel_index(rt, ARG(1), len, 0); i < len; i++)
        if (js_strict_eq(a->u.arr.v[i], ARG(0)))
            return jv_num(i);
    return jv_num(-1);
}

NATIVE(arr_last_index_of)
{
    jobj *a = array_like(rt, self);
    uint32_t len = a->u.arr.len;
    if (!len)
        return jv_num(-1);
    int64_t from = len - 1;
    if (argc > 1) {
        double d = js_to_integer(rt, argv[1]);
        from = d < 0 ? (int64_t)(len + d) : (d >= len ? len - 1 : (int64_t)d);
    }
    for (int64_t i = from; i >= 0; i--)
        if (js_strict_eq(a->u.arr.v[i], ARG(0)))
            return jv_num((double)i);
    return jv_num(-1);
}

NATIVE(arr_includes)
{
    jobj *a = array_like(rt, self);
    uint32_t len = a->u.arr.len;
    for (uint32_t i = rel_index(rt, ARG(1), len, 0); i < len; i++)
        if (js_same_value_zero(a->u.arr.v[i], ARG(0)))
            return jv_bool(1);
    return jv_bool(0);
}

/* Wspolne dla find, filter, map, forEach, some, every */
enum { IT_FOREACH, IT_MAP, IT_FILTER, IT_SOME, IT_EVERY, IT_FIND, IT_FINDINDEX, IT_FINDLAST, IT_FINDLASTINDEX };

static jv iterate(js_rt *rt, jv self, int argc, jv *argv, int kind)
{
    jobj *a = array_like(rt, self);
    jobj *f = callback(rt, ARG(0));
    jv this_arg = ARG(1);
    jobj *out = kind == IT_MAP ? js_new_array(rt, a->u.arr.len) : kind == IT_FILTER ? js_new_array(rt, 0) : 0;
    uint32_t len = a->u.arr.len;
    int backwards = kind == IT_FINDLAST || kind == IT_FINDLASTINDEX;
    for (uint32_t k = 0; k < len && k < a->u.arr.len + (backwards ? len : 0); k++) {
        uint32_t i = backwards ? len - 1 - k : k;
        if (i >= a->u.arr.len)
            continue;
        jv v = a->u.arr.v[i];
        jv args[3] = { v, jv_num(i), jv_obj(a) };
        jv r = js_call(rt, jv_obj(f), this_arg, 3, args);
        switch (kind) {
        case IT_MAP:
            if (i < out->u.arr.len)
                out->u.arr.v[i] = r;
            break;
        case IT_FILTER:
            if (js_truthy(r))
                js_array_push(rt, out, v);
            break;
        case IT_SOME:
            if (js_truthy(r))
                return jv_bool(1);
            break;
        case IT_EVERY:
            if (!js_truthy(r))
                return jv_bool(0);
            break;
        case IT_FIND:
        case IT_FINDLAST:
            if (js_truthy(r))
                return v;
            break;
        case IT_FINDINDEX:
        case IT_FINDLASTINDEX:
            if (js_truthy(r))
                return jv_num(i);
            break;
        }
    }
    switch (kind) {
    case IT_MAP:
    case IT_FILTER: return jv_obj(out);
    case IT_SOME: return jv_bool(0);
    case IT_EVERY: return jv_bool(1);
    case IT_FINDINDEX:
    case IT_FINDLASTINDEX: return jv_num(-1);
    }
    return jv_undef();
}

NATIVE(arr_for_each) { return iterate(rt, self, argc, argv, IT_FOREACH); }
NATIVE(arr_map) { return iterate(rt, self, argc, argv, IT_MAP); }
NATIVE(arr_filter) { return iterate(rt, self, argc, argv, IT_FILTER); }
NATIVE(arr_some) { return iterate(rt, self, argc, argv, IT_SOME); }
NATIVE(arr_every) { return iterate(rt, self, argc, argv, IT_EVERY); }
NATIVE(arr_find) { return iterate(rt, self, argc, argv, IT_FIND); }
NATIVE(arr_find_index) { return iterate(rt, self, argc, argv, IT_FINDINDEX); }
NATIVE(arr_find_last) { return iterate(rt, self, argc, argv, IT_FINDLAST); }
NATIVE(arr_find_last_index) { return iterate(rt, self, argc, argv, IT_FINDLASTINDEX); }

static jv reduce(js_rt *rt, jv self, int argc, jv *argv, int right)
{
    jobj *a = array_like(rt, self);
    jobj *f = callback(rt, ARG(0));
    uint32_t len = a->u.arr.len, k = 0;
    jv acc;
    if (argc > 1) {
        acc = argv[1];
    } else {
        if (!len)
            type_error(rt, "Reduce of empty array with no initial value");
        acc = a->u.arr.v[right ? len - 1 : 0];
        k = 1;
    }
    for (; k < len; k++) {
        uint32_t i = right ? len - 1 - k : k;
        if (i >= a->u.arr.len)
            continue;
        jv args[4] = { acc, a->u.arr.v[i], jv_num(i), jv_obj(a) };
        acc = js_call(rt, jv_obj(f), jv_undef(), 4, args);
    }
    return acc;
}

NATIVE(arr_reduce) { return reduce(rt, self, argc, argv, 0); }
NATIVE(arr_reduce_right) { return reduce(rt, self, argc, argv, 1); }

/* Sortowanie przez scalanie (stabilne, jak w nowych przegladarkach) */
struct sort_ctx {
    js_rt *rt;
    jv cmp;
    jstr **keys;                     /* bez funkcji porownujacej: teksty elementow */
};

static int sort_less_eq(struct sort_ctx *c, jv a, jv b, uint32_t ia, uint32_t ib)
{
    if (a.t == JT_UNDEF)
        return b.t == JT_UNDEF;
    if (b.t == JT_UNDEF)
        return 1;
    if (c->keys)
        return js_str_cmp(c->keys[ia], c->keys[ib]) <= 0;
    jv args[2] = { a, b };
    jv r = js_call(c->rt, c->cmp, jv_undef(), 2, args);
    double d = js_to_num(c->rt, r);
    return !(d > 0);
}

static void merge_sort(struct sort_ctx *c, jv *v, uint32_t *idx, jv *tmp, uint32_t *tidx, uint32_t n)
{
    if (n < 2)
        return;
    uint32_t h = n / 2;
    merge_sort(c, v, idx, tmp, tidx, h);
    merge_sort(c, v + h, idx + h, tmp, tidx, n - h);
    uint32_t i = 0, j = h, k = 0;
    while (i < h && j < n) {
        if (sort_less_eq(c, v[i], v[j], idx[i], idx[j])) {
            tmp[k] = v[i];
            tidx[k++] = idx[i++];
        } else {
            tmp[k] = v[j];
            tidx[k++] = idx[j++];
        }
    }
    while (i < h) {
        tmp[k] = v[i];
        tidx[k++] = idx[i++];
    }
    while (j < n) {
        tmp[k] = v[j];
        tidx[k++] = idx[j++];
    }
    memcpy(v, tmp, sizeof(jv) * n);
    memcpy(idx, tidx, sizeof(uint32_t) * n);
}

NATIVE(arr_sort)
{
    jobj *a = this_array(rt, self);
    uint32_t n = a->u.arr.len;
    if (n < 2)
        return self;
    struct sort_ctx c = { rt, ARG(0), 0 };
    if (c.cmp.t != JT_UNDEF && !js_is_callable(c.cmp))
        type_error(rt, "The comparison function must be either a function or undefined");
    uint32_t *idx = js_alloc(rt, sizeof(uint32_t) * n), *tidx = js_alloc(rt, sizeof(uint32_t) * n);
    jv *tmp = js_alloc(rt, sizeof(jv) * n);
    for (uint32_t i = 0; i < n; i++)
        idx[i] = i;
    if (c.cmp.t == JT_UNDEF) {
        c.keys = js_alloc(rt, sizeof(jstr *) * n);
        for (uint32_t i = 0; i < n; i++)
            c.keys[i] = a->u.arr.v[i].t == JT_UNDEF ? rt->empty : js_to_str(rt, a->u.arr.v[i]);
    }
    jv *copy = js_alloc(rt, sizeof(jv) * n);
    memcpy(copy, a->u.arr.v, sizeof(jv) * n);
    merge_sort(&c, copy, idx, tmp, tidx, n);
    if (a->u.arr.len >= n)
        memcpy(a->u.arr.v, copy, sizeof(jv) * n);
    return self;
}

NATIVE(arr_fill)
{
    jobj *a = this_array(rt, self);
    uint32_t len = a->u.arr.len;
    uint32_t from = rel_index(rt, ARG(1), len, 0), to = rel_index(rt, ARG(2), len, len);
    for (uint32_t i = from; i < to; i++)
        a->u.arr.v[i] = ARG(0);
    return self;
}

NATIVE(arr_keys)
{
    jobj *a = array_like(rt, self), *out = js_new_array(rt, 0);
    for (uint32_t i = 0; i < a->u.arr.len; i++)
        js_array_push(rt, out, jv_num(i));
    return jv_obj(out);
}

NATIVE(arr_values) { return jv_obj(js_iterate_to_array(rt, self).u.o); }

NATIVE(arr_entries)
{
    jobj *a = array_like(rt, self), *out = js_new_array(rt, 0);
    for (uint32_t i = 0; i < a->u.arr.len; i++) {
        jobj *pair = js_new_array(rt, 0);
        js_array_push(rt, pair, jv_num(i));
        js_array_push(rt, pair, a->u.arr.v[i]);
        js_array_push(rt, out, jv_obj(pair));
    }
    return jv_obj(out);
}

static void flatten(js_rt *rt, jobj *out, jobj *a, double depth)
{
    for (uint32_t i = 0; i < a->u.arr.len; i++) {
        jv v = a->u.arr.v[i];
        if (depth >= 1 && v.t == JT_OBJ && v.u.o->cls == JC_ARRAY)
            flatten(rt, out, v.u.o, depth - 1);
        else
            js_array_push(rt, out, v);
    }
}

NATIVE(arr_flat)
{
    jobj *a = array_like(rt, self), *out = js_new_array(rt, 0);
    double depth = ARG(0).t == JT_UNDEF ? 1 : js_to_integer(rt, argv[0]);
    if (depth > 100)
        depth = 100;
    flatten(rt, out, a, depth);
    return jv_obj(out);
}

NATIVE(arr_flat_map)
{
    jv mapped = iterate(rt, self, argc, argv, IT_MAP);
    jobj *out = js_new_array(rt, 0);
    flatten(rt, out, mapped.u.o, 1);
    return jv_obj(out);
}

NATIVE(arr_at)
{
    jobj *a = array_like(rt, self);
    double d = js_to_integer(rt, ARG(0));
    if (d < 0)
        d += a->u.arr.len;
    if (d < 0 || d >= a->u.arr.len)
        return jv_undef();
    return a->u.arr.v[(uint32_t)d];
}

NATIVE(arr_copy_within)
{
    jobj *a = this_array(rt, self);
    uint32_t len = a->u.arr.len;
    uint32_t to = rel_index(rt, ARG(0), len, 0), from = rel_index(rt, ARG(1), len, 0), end = rel_index(rt, ARG(2), len, len);
    if (end > from) {
        uint32_t n = end - from;
        if (to + n > len)
            n = len - to;
        memmove(a->u.arr.v + to, a->u.arr.v + from, sizeof(jv) * n);
    }
    return self;
}

/* ---------- Error ---------- */

static jv error_ctor_kind(js_rt *rt, int kind, int argc, jv *argv)
{
    jobj *e = js_make_error(rt, kind, ARG(0).t == JT_UNDEF ? 0 : js_to_str(rt, argv[0]));
    if (argc > 1 && argv[1].t == JT_OBJ && js_has(rt, argv[1].u.o, rt->A.cause))
        js_define(rt, e, rt->A.cause, js_get(rt, argv[1], rt->A.cause), PF_WRITE | PF_CONF);
    return jv_obj(e);
}

static jv error_ctor(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn)
{
    int kind = (int)fn->u.nf.data.u.n;
    return error_ctor_kind(rt, kind, argc, argv);
}

NATIVE(errp_to_string)
{
    if (self.t != JT_OBJ)
        type_error(rt, "Error.prototype.toString called on non-object");
    jv name = js_get(rt, self, rt->A.name), msg = js_get(rt, self, rt->A.message);
    jstr *n = name.t == JT_UNDEF ? S(rt, "Error") : js_to_str(rt, name);
    jstr *m = msg.t == JT_UNDEF ? rt->empty : js_to_str(rt, msg);
    if (!n->len)
        return jv_str(m);
    if (!m->len)
        return jv_str(n);
    return jv_str(js_concat(rt, js_concat(rt, n, S(rt, ": ")), m));
}

NATIVE(err_capture_stack) { return jv_undef(); }

/* ---------- Boolean, Number ---------- */

static jv boolean_call(js_rt *rt U, jv self U, int argc, jv *argv, jobj *fn U) { return jv_bool(js_truthy(ARG(0))); }

static jv boolean_ctor(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn U)
{
    return jv_obj(js_to_obj(rt, jv_bool(js_truthy(ARG(0)))));
}

static jv this_prim(js_rt *rt, jv self, int t, int cls, const char *name)
{
    if (self.t == t)
        return self;
    if (self.t == JT_OBJ && self.u.o->cls == cls)
        return self.u.o->u.prim;
    js_throw_error(rt, JS_ERR_TYPE, "%s.prototype.valueOf requires that 'this' be a %s", name, name);
}

NATIVE(boolp_to_string) { return jv_str(js_atom(rt, this_prim(rt, self, JT_BOOL, JC_BOOL, "Boolean").u.b ? "true" : "false")); }
NATIVE(boolp_value_of) { return this_prim(rt, self, JT_BOOL, JC_BOOL, "Boolean"); }

static jv number_call(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn U)
{
    return jv_num(argc ? js_to_num(rt, argv[0]) : 0);
}

static jv number_ctor(js_rt *rt, jv self U, int argc, jv *argv, jobj *fn U)
{
    return jv_obj(js_to_obj(rt, jv_num(argc ? js_to_num(rt, argv[0]) : 0)));
}

static double this_num(js_rt *rt, jv self) { return this_prim(rt, self, JT_NUM, JC_NUM, "Number").u.n; }

NATIVE(nump_to_string)
{
    double d = this_num(rt, self);
    int radix = ARG(0).t == JT_UNDEF ? 10 : js_to_int32(rt, argv[0]);
    if (radix < 2 || radix > 36)
        js_throw_error(rt, JS_ERR_RANGE, "toString() radix must be between 2 and 36");
    char buf[1200];
    js_radix(d, radix, buf, sizeof(buf));
    return jv_str(S(rt, buf));
}

NATIVE(nump_to_fixed)
{
    double d = this_num(rt, self);
    double f = js_to_integer(rt, ARG(0));
    if (f < 0 || f > 100)
        js_throw_error(rt, JS_ERR_RANGE, "toFixed() digits argument must be between 0 and 100");
    char buf[500];
    js_fixed(d, (int)f, buf, sizeof(buf));
    return jv_str(S(rt, buf));
}

NATIVE(nump_to_precision)
{
    double d = this_num(rt, self);
    if (ARG(0).t == JT_UNDEF)
        return jv_str(js_to_str(rt, jv_num(d)));
    double p = js_to_integer(rt, argv[0]);
    if (p < 1 || p > 100)
        js_throw_error(rt, JS_ERR_RANGE, "toPrecision() argument must be between 1 and 100");
    char buf[500];
    js_precision(d, (int)p, buf, sizeof(buf));
    return jv_str(S(rt, buf));
}

NATIVE(nump_to_exponential)
{
    double d = this_num(rt, self);
    int digits = ARG(0).t == JT_UNDEF ? -1 : (int)js_to_integer(rt, argv[0]);
    if (digits > 100)
        js_throw_error(rt, JS_ERR_RANGE, "toExponential() argument must be between 0 and 100");
    char buf[500];
    js_exponential(d, digits, buf, sizeof(buf));
    return jv_str(S(rt, buf));
}

/* 1234567.891 -> "1,234,567.891" (albo po polsku "1 234 567,891") */
static jstr *locale_number(js_rt *rt, double d, int polish, int max_frac)
{
    if (!js_isfinite(d))
        return js_to_str(rt, jv_num(d));
    char buf[500], out[600];
    js_fixed(d, max_frac, buf, sizeof(buf));
    char *dot = buf;
    while (*dot && *dot != '.')
        dot++;
    if (*dot) {                                      /* bez zer na koncu */
        char *e = dot + strlen(dot) - 1;
        while (e > dot && *e == '0')
            *e-- = '\0';
        if (e == dot)
            *e = '\0';
    }
    const char *p = buf;
    unsigned n = 0;
    if (*p == '-')
        out[n++] = *p++;
    unsigned intlen = 0;
    while (p[intlen] && p[intlen] != '.')
        intlen++;
    for (unsigned i = 0; i < intlen; i++) {
        out[n++] = p[i];
        unsigned left = intlen - 1 - i;
        if (left && left % 3 == 0 && !(polish && intlen == 4)) {
            if (polish) {
                out[n++] = (char)0xC2;               /* twarda spacja */
                out[n++] = (char)0xA0;
            } else {
                out[n++] = ',';
            }
        }
    }
    if (p[intlen] == '.') {
        out[n++] = polish ? ',' : '.';
        for (const char *f = p + intlen + 1; *f; f++)
            out[n++] = *f;
    }
    out[n] = '\0';
    return js_str_utf8(rt, out, n);
}

static int locale_is_polish(js_rt *rt, jv loc)
{
    if (loc.t != JT_STR)
        return 0;
    (void)rt;
    return loc.u.s->len >= 2 && loc.u.s->c[0] == 'p' && loc.u.s->c[1] == 'l';
}

NATIVE(nump_to_locale_string)
{
    double d = this_num(rt, self);
    int frac = 3;
    if (ARG(1).t == JT_OBJ) {
        jv mx = js_get(rt, argv[1], js_atom(rt, "maximumFractionDigits"));
        if (mx.t == JT_NUM && mx.u.n >= 0 && mx.u.n <= 20)
            frac = (int)mx.u.n;
    }
    return jv_str(locale_number(rt, d, locale_is_polish(rt, ARG(0)), frac));
}

NATIVE(nump_value_of) { return jv_num(this_num(rt, self)); }

NATIVE(num_is_integer) { return jv_bool(ARG(0).t == JT_NUM && js_isfinite(argv[0].u.n) && js_trunc(argv[0].u.n) == argv[0].u.n); }
NATIVE(num_is_safe_integer)
{
    return jv_bool(ARG(0).t == JT_NUM && js_isfinite(argv[0].u.n) && js_trunc(argv[0].u.n) == argv[0].u.n &&
                   js_fabs(argv[0].u.n) <= 9007199254740991.0);
}
NATIVE(num_is_finite) { return jv_bool(ARG(0).t == JT_NUM && js_isfinite(argv[0].u.n)); }
NATIVE(num_is_nan) { return jv_bool(ARG(0).t == JT_NUM && js_isnan(argv[0].u.n)); }

/* ---------- Math ---------- */

static double num_arg(js_rt *rt, int argc, jv *argv, int i)
{
    if (i >= argc)
        return js_nan;
    return argv[i].t == JT_NUM ? argv[i].u.n : js_to_num(rt, argv[i]);
}

#define MATH1(name, expr) NATIVE(name) { double x = num_arg(rt, argc, argv, 0); return jv_num(expr); }
MATH1(m_abs, js_fabs(x))
MATH1(m_floor, js_floor(x))
MATH1(m_ceil, js_ceil(x))
MATH1(m_round, js_round(x))
MATH1(m_trunc, js_trunc(x))
MATH1(m_sign, js_isnan(x) ? x : x > 0 ? 1 : x < 0 ? -1 : x)
MATH1(m_sqrt, x < 0 ? js_nan : js_sqrt(x))
MATH1(m_cbrt, js_cbrt(x))
MATH1(m_exp, js_exp(x))
MATH1(m_expm1, js_expm1(x))
MATH1(m_log, js_log(x))
MATH1(m_log1p, js_log1p(x))
MATH1(m_sin, js_sin(x))
MATH1(m_cos, js_cos(x))
MATH1(m_tan, js_tan(x))
MATH1(m_asin, js_asin(x))
MATH1(m_acos, js_acos(x))
MATH1(m_atan, js_atan(x))
MATH1(m_sinh, (js_exp(x) - js_exp(-x)) / 2)
MATH1(m_cosh, (js_exp(x) + js_exp(-x)) / 2)
MATH1(m_tanh, x > 20 ? 1.0 : x < -20 ? -1.0 : js_expm1(2 * x) / (js_expm1(2 * x) + 2))
MATH1(m_asinh, x < 0 ? -js_log(-x + js_sqrt(x * x + 1)) : js_log(x + js_sqrt(x * x + 1)))
MATH1(m_acosh, x < 1 ? js_nan : js_log(x + js_sqrt(x * x - 1)))
MATH1(m_atanh, 0.5 * js_log1p(2 * x / (1 - x)))
MATH1(m_fround, (double)(float)x)

NATIVE(m_log10)
{
    double x = num_arg(rt, argc, argv, 0);
    double r = js_log(x) * 0.43429448190325182765;
    double k = js_round(r);
    if (js_fabs(r - k) < 1e-9 && k >= -300 && k <= 300 && js_pow(10, k) == x)
        return jv_num(k);
    return jv_num(r);
}

NATIVE(m_log2)
{
    double x = num_arg(rt, argc, argv, 0);
    double r = js_log(x) * 1.44269504088896340736;
    double k = js_round(r);
    if (js_fabs(r - k) < 1e-9 && k >= -1074 && k <= 1023 && js_ldexp(1, (int)k) == x)
        return jv_num(k);
    return jv_num(r);
}

NATIVE(m_pow) { return jv_num(js_pow(num_arg(rt, argc, argv, 0), num_arg(rt, argc, argv, 1))); }
NATIVE(m_atan2) { return jv_num(js_atan2(num_arg(rt, argc, argv, 0), num_arg(rt, argc, argv, 1))); }

NATIVE(m_min)
{
    double r = js_inf;
    for (int i = 0; i < argc; i++) {
        double x = num_arg(rt, argc, argv, i);
        if (js_isnan(x) || js_isnan(r))
            r = js_nan;
        else if (x < r || (x == 0 && r == 0 && 1 / x < 0))
            r = x;
    }
    return jv_num(r);
}

NATIVE(m_max)
{
    double r = -js_inf;
    for (int i = 0; i < argc; i++) {
        double x = num_arg(rt, argc, argv, i);
        if (js_isnan(x) || js_isnan(r))
            r = js_nan;
        else if (x > r || (x == 0 && r == 0 && 1 / x > 0))
            r = x;
    }
    return jv_num(r);
}

NATIVE(m_hypot)
{
    double m = 0;
    for (int i = 0; i < argc; i++) {
        double x = js_fabs(num_arg(rt, argc, argv, i));
        if (js_isinf(x))
            return jv_num(js_inf);
        if (x > m)
            m = x;
    }
    if (m == 0)
        return jv_num(0);
    double s = 0;
    for (int i = 0; i < argc; i++) {
        double x = num_arg(rt, argc, argv, i) / m;
        s += x * x;
    }
    return jv_num(m * js_sqrt(s));
}

NATIVE(m_random)
{
    /* xorshift128+ - szybki generator liczb losowych */
    uint64_t s1 = rt->rand_state[0];
    const uint64_t s0 = rt->rand_state[1];
    rt->rand_state[0] = s0;
    s1 ^= s1 << 23;
    rt->rand_state[1] = s1 ^ s0 ^ (s1 >> 17) ^ (s0 >> 26);
    uint64_t r = rt->rand_state[1] + s0;
    return jv_num((double)(r >> 11) * (1.0 / 9007199254740992.0));
}

NATIVE(m_clz32)
{
    uint32_t x = js_to_uint32(rt, ARG(0));
    int n = 0;
    if (!x)
        return jv_num(32);
    while (!(x & 0x80000000u)) {
        x <<= 1;
        n++;
    }
    return jv_num(n);
}

NATIVE(m_imul) { return jv_num((int32_t)((uint32_t)js_to_int32(rt, ARG(0)) * (uint32_t)js_to_int32(rt, ARG(1)))); }

/* ---------- JSON ---------- */

struct json_p {
    js_rt *rt;
    const jstr *s;
    uint32_t i;
};

static void json_error(struct json_p *p) __attribute__((noreturn));
static void json_error(struct json_p *p)
{
    if (p->i >= p->s->len)
        js_throw_error(p->rt, JS_ERR_SYNTAX, "Unexpected end of JSON input");
    char c[8];
    jstr one = { (uint16_t *)&p->s->c[p->i], 1, 0, 0, 0, 0, 0 };
    js_utf8(&one, c, sizeof(c));
    js_throw_error(p->rt, JS_ERR_SYNTAX, "Unexpected token '%s' in JSON at position %u", c, p->i);
}

static void json_ws(struct json_p *p)
{
    while (p->i < p->s->len && (p->s->c[p->i] == ' ' || p->s->c[p->i] == '\t' || p->s->c[p->i] == '\n' || p->s->c[p->i] == '\r'))
        p->i++;
}

static jstr *json_string(struct json_p *p)
{
    struct jsb b;
    jsb_init(&b, p->rt);
    p->i++;
    for (;;) {
        if (p->i >= p->s->len)
            json_error(p);
        uint16_t c = p->s->c[p->i++];
        if (c == '"')
            break;
        if (c < 0x20) {
            p->i--;
            json_error(p);
        }
        if (c == '\\') {
            if (p->i >= p->s->len)
                json_error(p);
            c = p->s->c[p->i++];
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case '"': case '\\': case '/': break;
            case 'u': {
                uint16_t v = 0;
                for (int k = 0; k < 4; k++) {
                    int h = p->i < p->s->len ? hexd(p->s->c[p->i]) : -1;
                    if (h < 0)
                        json_error(p);
                    v = (uint16_t)(v * 16 + h);
                    p->i++;
                }
                c = v;
                break;
            }
            default:
                p->i--;
                json_error(p);
            }
        }
        jsb_char(&b, c);
    }
    return jsb_done(&b);
}

static jv json_value(struct json_p *p, int depth)
{
    js_check_stack(p->rt);
    if (depth > 500)
        js_throw_error(p->rt, JS_ERR_RANGE, "JSON nested too deeply");
    json_ws(p);
    if (p->i >= p->s->len)
        json_error(p);
    uint16_t c = p->s->c[p->i];
    if (c == '{') {
        jobj *o = js_new_object(p->rt, p->rt->object_proto);
        p->i++;
        json_ws(p);
        if (p->i < p->s->len && p->s->c[p->i] == '}') {
            p->i++;
            return jv_obj(o);
        }
        for (;;) {
            json_ws(p);
            if (p->i >= p->s->len || p->s->c[p->i] != '"')
                json_error(p);
            jstr *key = js_intern(p->rt, json_string(p));
            json_ws(p);
            if (p->i >= p->s->len || p->s->c[p->i] != ':')
                json_error(p);
            p->i++;
            jv v = json_value(p, depth + 1);
            js_define(p->rt, o, key, v, PF_ALL);
            json_ws(p);
            if (p->i < p->s->len && p->s->c[p->i] == ',') {
                p->i++;
                continue;
            }
            if (p->i < p->s->len && p->s->c[p->i] == '}') {
                p->i++;
                return jv_obj(o);
            }
            json_error(p);
        }
    }
    if (c == '[') {
        jobj *a = js_new_array(p->rt, 0);
        p->i++;
        json_ws(p);
        if (p->i < p->s->len && p->s->c[p->i] == ']') {
            p->i++;
            return jv_obj(a);
        }
        for (;;) {
            js_array_push(p->rt, a, json_value(p, depth + 1));
            json_ws(p);
            if (p->i < p->s->len && p->s->c[p->i] == ',') {
                p->i++;
                continue;
            }
            if (p->i < p->s->len && p->s->c[p->i] == ']') {
                p->i++;
                return jv_obj(a);
            }
            json_error(p);
        }
    }
    if (c == '"')
        return jv_str(json_string(p));
    if (c == '-' || (c >= '0' && c <= '9')) {
        char buf[400];
        unsigned n = 0;
        while (p->i < p->s->len && n < sizeof(buf) - 1) {
            uint16_t d = p->s->c[p->i];
            if ((d >= '0' && d <= '9') || d == '-' || d == '+' || d == '.' || d == 'e' || d == 'E') {
                buf[n++] = (char)d;
                p->i++;
            } else {
                break;
            }
        }
        buf[n] = '\0';
        const char *end;
        double v = js_strtod(buf, &end);
        if (!end || *end)
            json_error(p);
        return jv_num(v);
    }
    static const struct { const char *w; int t; } words[] = { { "true", 1 }, { "false", 2 }, { "null", 3 } };
    for (int k = 0; k < 3; k++) {
        const char *w = words[k].w;
        unsigned l = (unsigned)strlen(w), m = 0;
        while (m < l && p->i + m < p->s->len && p->s->c[p->i + m] == (uint8_t)w[m])
            m++;
        if (m == l) {
            p->i += l;
            return words[k].t == 1 ? jv_bool(1) : words[k].t == 2 ? jv_bool(0) : jv_null();
        }
    }
    json_error(p);
}

static jv json_revive(js_rt *rt, jv holder, jstr *key, jv reviver)
{
    jv v = js_get(rt, holder, key);
    if (v.t == JT_OBJ) {
        jobj *keys = js_own_keys(rt, v.u.o, 1);
        for (uint32_t i = 0; i < keys->u.arr.len; i++) {
            jstr *k = keys->u.arr.v[i].u.s;
            jv nv = json_revive(rt, v, k, reviver);
            if (nv.t == JT_UNDEF)
                js_delete(rt, v.u.o, k);
            else
                js_set(rt, v, k, nv);
        }
    }
    jv args[2] = { jv_str(key), v };
    return js_call(rt, reviver, holder, 2, args);
}

NATIVE(json_parse)
{
    struct json_p p = { rt, js_to_str(rt, ARG(0)), 0 };
    jv v = json_value(&p, 0);
    json_ws(&p);
    if (p.i < p.s->len)
        json_error(&p);
    if (js_is_callable(ARG(1))) {
        jobj *root = js_new_object(rt, rt->object_proto);
        js_define(rt, root, rt->empty, v, PF_ALL);
        return json_revive(rt, jv_obj(root), rt->empty, argv[1]);
    }
    return v;
}

struct json_s {
    js_rt *rt;
    struct jsb b;
    jstr *indent;
    jv replacer;
    jobj *allow;                     /* tablica dozwolonych kluczy */
    jobj *stack[64];
    int depth;
};

static void json_quote(struct jsb *b, jstr *s)
{
    static const char hex[] = "0123456789abcdef";
    jsb_char(b, '"');
    for (uint32_t i = 0; i < s->len; i++) {
        uint16_t c = s->c[i];
        switch (c) {
        case '"': jsb_ascii(b, "\\\""); break;
        case '\\': jsb_ascii(b, "\\\\"); break;
        case '\n': jsb_ascii(b, "\\n"); break;
        case '\r': jsb_ascii(b, "\\r"); break;
        case '\t': jsb_ascii(b, "\\t"); break;
        case '\b': jsb_ascii(b, "\\b"); break;
        case '\f': jsb_ascii(b, "\\f"); break;
        default:
            if (c < 0x20) {
                jsb_ascii(b, "\\u00");
                jsb_char(b, (uint16_t)hex[c >> 4]);
                jsb_char(b, (uint16_t)hex[c & 15]);
            } else {
                jsb_char(b, c);
            }
        }
    }
    jsb_char(b, '"');
}

static void json_newline(struct json_s *j)
{
    if (!j->indent->len)
        return;
    jsb_char(&j->b, '\n');
    for (int i = 0; i < j->depth; i++)
        jsb_str(&j->b, j->indent);
}

/* 0 = nic nie wypisano (undefined, funkcja) */
static int json_write(struct json_s *j, jv holder, jstr *key, jv v)
{
    js_rt *rt = j->rt;
    js_check_stack(rt);
    if (v.t == JT_OBJ) {
        jv tj = js_get(rt, v, rt->A.toJSON);
        if (js_is_callable(tj)) {
            jv a = jv_str(key);
            v = js_call(rt, tj, v, 1, &a);
        }
    }
    if (js_is_callable(j->replacer)) {
        jv args[2] = { jv_str(key), v };
        v = js_call(rt, j->replacer, holder, 2, args);
    }
    if (v.t == JT_OBJ) {
        int c = v.u.o->cls;
        if (c == JC_NUM || c == JC_STR || c == JC_BOOL)
            v = v.u.o->u.prim;
    }
    switch (v.t) {
    case JT_NULL: jsb_ascii(&j->b, "null"); return 1;
    case JT_BOOL: jsb_ascii(&j->b, v.u.b ? "true" : "false"); return 1;
    case JT_NUM:
        if (js_isfinite(v.u.n))
            jsb_num(&j->b, v.u.n);
        else
            jsb_ascii(&j->b, "null");
        return 1;
    case JT_STR: json_quote(&j->b, v.u.s); return 1;
    case JT_UNDEF: return 0;
    }
    if (js_is_callable(v))
        return 0;
    jobj *o = v.u.o;
    for (int i = 0; i < j->depth; i++)
        if (j->stack[i] == o)
            type_error(rt, "Converting circular structure to JSON");
    if (j->depth >= 64)
        js_throw_error(rt, JS_ERR_RANGE, "JSON nested too deeply");
    j->stack[j->depth++] = o;
    if (o->cls == JC_ARRAY) {
        jsb_char(&j->b, '[');
        for (uint32_t i = 0; i < o->u.arr.len; i++) {
            if (i)
                jsb_char(&j->b, ',');
            json_newline(j);
            if (!json_write(j, v, js_key_of(rt, jv_num(i)), i < o->u.arr.len ? o->u.arr.v[i] : jv_undef()))
                jsb_ascii(&j->b, "null");
        }
        j->depth--;
        if (o->u.arr.len)
            json_newline(j);
        jsb_char(&j->b, ']');
        return 1;
    }
    jsb_char(&j->b, '{');
    jobj *keys = j->allow ? j->allow : js_own_keys(rt, o, 1);
    int any = 0;
    for (uint32_t i = 0; i < keys->u.arr.len; i++) {
        jstr *k = js_key_of(rt, keys->u.arr.v[i]);
        if (j->allow && !js_has(rt, o, k))
            continue;
        jv val = js_get(rt, v, k);
        uint32_t mark = j->b.len;
        if (any)
            jsb_char(&j->b, ',');
        json_newline(j);
        json_quote(&j->b, k);
        jsb_char(&j->b, ':');
        if (j->indent->len)
            jsb_char(&j->b, ' ');
        if (json_write(j, v, k, val))
            any = 1;
        else
            j->b.len = mark;                        /* undefined - bez tego klucza */
    }
    j->depth--;
    if (any)
        json_newline(j);
    jsb_char(&j->b, '}');
    return 1;
}

jstr *js_json_stringify(js_rt *rt, jv v, jv indent)
{
    struct json_s j;
    memset(&j, 0, sizeof(j));
    j.rt = rt;
    jsb_init(&j.b, rt);
    j.indent = rt->empty;
    if (indent.t == JT_NUM) {
        int n = (int)indent.u.n;
        if (n > 10) n = 10;
        struct jsb ib;
        jsb_init(&ib, rt);
        for (int i = 0; i < n; i++)
            jsb_char(&ib, ' ');
        j.indent = jsb_done(&ib);
    } else if (indent.t == JT_STR) {
        j.indent = js_substr(rt, indent.u.s, 0, 10);
    }
    jobj *holder = js_new_object(rt, rt->object_proto);
    js_define(rt, holder, rt->empty, v, PF_ALL);
    if (!json_write(&j, jv_obj(holder), rt->empty, v))
        return 0;
    return jsb_done(&j.b);
}

NATIVE(json_stringify)
{
    struct json_s j;
    memset(&j, 0, sizeof(j));
    j.rt = rt;
    jsb_init(&j.b, rt);
    j.indent = rt->empty;
    jv indent = ARG(2);
    if (indent.t == JT_OBJ)
        indent = js_to_prim(rt, indent, 0);
    if (indent.t == JT_NUM) {
        int n = (int)indent.u.n;
        if (n > 10) n = 10;
        struct jsb ib;
        jsb_init(&ib, rt);
        for (int i = 0; i < n; i++)
            jsb_char(&ib, ' ');
        j.indent = jsb_done(&ib);
    } else if (indent.t == JT_STR) {
        j.indent = js_substr(rt, indent.u.s, 0, 10);
    }
    if (js_is_callable(ARG(1)))
        j.replacer = argv[1];
    else if (ARG(1).t == JT_OBJ && argv[1].u.o->cls == JC_ARRAY)
        j.allow = argv[1].u.o;
    jobj *holder = js_new_object(rt, rt->object_proto);
    js_define(rt, holder, rt->empty, ARG(0), PF_ALL);
    if (!json_write(&j, jv_obj(holder), rt->empty, ARG(0)))
        return jv_undef();
    return jv_str(jsb_done(&j.b));
}

/* ---------- console ---------- */

static void inspect_into(js_rt *rt, struct jsb *b, jv v, int depth, int nested);

static int is_ident_key(jstr *k)
{
    if (!k->len)
        return 0;
    for (uint32_t i = 0; i < k->len; i++) {
        uint16_t c = k->c[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 128 ||
              (i && c >= '0' && c <= '9')))
            return 0;
    }
    return 1;
}

static void inspect_into(js_rt *rt, struct jsb *b, jv v, int depth, int nested)
{
    if (b->len > 20000) {
        jsb_ascii(b, "...");
        return;
    }
    switch (v.t) {
    case JT_UNDEF: jsb_ascii(b, "undefined"); return;
    case JT_NULL: jsb_ascii(b, "null"); return;
    case JT_BOOL: jsb_ascii(b, v.u.b ? "true" : "false"); return;
    case JT_NUM:
        if (v.u.n == 0 && 1 / v.u.n < 0)
            jsb_ascii(b, "-0");
        else
            jsb_num(b, v.u.n);
        return;
    case JT_STR:
        if (nested) {
            jsb_char(b, '\'');
            jsb_str(b, v.u.s);
            jsb_char(b, '\'');
        } else {
            jsb_str(b, v.u.s);
        }
        return;
    }
    jobj *o = v.u.o;
    if (js_is_callable(v)) {
        jstr *n = js_func_name(rt, o);
        jsb_ascii(b, (o->flags & OF_CLASS) ? "[class " : "[Function");
        if (n->len) {
            if (!(o->flags & OF_CLASS))
                jsb_ascii(b, ": ");
            jsb_str(b, n);
        } else if (!(o->flags & OF_CLASS)) {
            jsb_ascii(b, " (anonymous)");
        }
        jsb_char(b, ']');
        return;
    }
    switch (o->cls) {
    case JC_ERROR: {
        jv st = js_get(rt, v, rt->A.stack);
        if (st.t == JT_STR && !nested) {
            jsb_str(b, st.u.s);
        } else {
            jv name = js_get(rt, v, rt->A.name), msg = js_get(rt, v, rt->A.message);
            if (nested)
                jsb_char(b, '[');
            jsb_str(b, name.t == JT_STR ? name.u.s : js_atom(rt, "Error"));
            if (msg.t == JT_STR && msg.u.s->len) {
                jsb_ascii(b, ": ");
                jsb_str(b, msg.u.s);
            }
            if (nested)
                jsb_char(b, ']');
        }
        return;
    }
    case JC_DATE: {
        jv s = js_call(rt, js_get(rt, v, js_atom(rt, "toISOString")), v, 0, 0);
        jsb_str(b, js_to_str(rt, s));
        return;
    }
    case JC_REGEXP:
        jsb_str(b, js_to_str(rt, v));
        return;
    case JC_NUM: case JC_STR: case JC_BOOL:
        jsb_ascii(b, "[");
        jsb_ascii(b, o->cls == JC_NUM ? "Number: " : o->cls == JC_STR ? "String: " : "Boolean: ");
        inspect_into(rt, b, o->u.prim, depth, 1);
        jsb_ascii(b, "]");
        return;
    case JC_NODE: {
        jv s = js_get(rt, v, js_atom(rt, "nodeName"));
        jsb_char(b, '<');
        if (s.t == JT_STR)
            jsb_str(b, s.u.s);
        jv id = js_get(rt, v, rt->A.id);
        if (id.t == JT_STR && id.u.s->len) {
            jsb_char(b, '#');
            jsb_str(b, id.u.s);
        }
        jsb_char(b, '>');
        return;
    }
    }
    for (int i = 0; i < rt->join_depth; i++)
        if (rt->join_stack[i] == o) {
            jsb_ascii(b, "[Circular]");
            return;
        }
    if (depth > 2) {
        jsb_ascii(b, o->cls == JC_ARRAY ? "[Array]" : "[Object]");
        return;
    }
    if (rt->join_depth >= 32) {
        jsb_ascii(b, "...");
        return;
    }
    rt->join_stack[rt->join_depth++] = o;
    if (o->cls == JC_ARRAY || o->cls == JC_ARGS) {
        jsb_ascii(b, o->cls == JC_ARGS ? "[Arguments] [ " : "[ ");
        if (!o->u.arr.len)
            b->len--;
        for (uint32_t i = 0; i < o->u.arr.len && i < 100; i++) {
            if (i)
                jsb_ascii(b, ", ");
            inspect_into(rt, b, o->u.arr.v[i], depth + 1, 1);
        }
        if (o->u.arr.len > 100)
            jsb_ascii(b, ", ...");
        jsb_ascii(b, o->u.arr.len ? " ]" : "]");
    } else if (o->cls == JC_MAP || o->cls == JC_SET) {
        char head[32];
        ksnprintf(head, sizeof(head), "%s(%u) {", o->cls == JC_MAP ? "Map" : "Set", o->u.map.live);
        jsb_ascii(b, head);
        int first = 1;
        for (uint32_t i = 0; i < o->u.map.n; i++) {
            struct jmapent *e = &o->u.map.e[i];
            if (e->deleted)
                continue;
            jsb_ascii(b, first ? " " : ", ");
            first = 0;
            inspect_into(rt, b, e->key, depth + 1, 1);
            if (o->cls == JC_MAP) {
                jsb_ascii(b, " => ");
                inspect_into(rt, b, e->value, depth + 1, 1);
            }
        }
        jsb_ascii(b, first ? "}" : " }");
    } else {
        jobj *keys = js_own_keys(rt, o, 1);
        if (o->proto && o->proto != rt->object_proto) {
            jv c = js_get(rt, jv_obj(o->proto), rt->A.constructor);
            if (js_is_callable(c)) {
                jstr *n = js_func_name(rt, c.u.o);
                if (n->len && !js_str_is(n, "Object")) {
                    jsb_str(b, n);
                    jsb_char(b, ' ');
                }
            }
        } else if (o->cls == JC_PROMISE) {
            jsb_ascii(b, "Promise ");
        }
        if (!keys->u.arr.len) {
            jsb_ascii(b, "{}");
        } else {
            jsb_ascii(b, "{ ");
            for (uint32_t i = 0; i < keys->u.arr.len && i < 50; i++) {
                jstr *k = keys->u.arr.v[i].u.s;
                if (i)
                    jsb_ascii(b, ", ");
                if (is_ident_key(k)) {
                    jsb_str(b, k);
                } else {
                    jsb_char(b, '\'');
                    jsb_str(b, k);
                    jsb_char(b, '\'');
                }
                jsb_ascii(b, ": ");
                struct jprop *p = js_own(o, k);
                if (p && p->v.t == JT_ACC)
                    jsb_ascii(b, "[Getter/Setter]");
                else
                    inspect_into(rt, b, js_get(rt, v, k), depth + 1, 1);
            }
            if (keys->u.arr.len > 50)
                jsb_ascii(b, ", ...");
            jsb_ascii(b, " }");
        }
    }
    rt->join_depth--;
}

jstr *js_inspect(js_rt *rt, jv v, int depth)
{
    struct jsb b;
    jsb_init(&b, rt);
    inspect_into(rt, &b, v, depth, 0);
    return jsb_done(&b);
}

/* Tekst dla console.log: argumenty oddzielone spacja, %s %d %o %c w pierwszym */
static jstr *format_args(js_rt *rt, int argc, jv *argv)
{
    struct jsb b;
    jsb_init(&b, rt);
    int i = 0;
    if (argc && argv[0].t == JT_STR) {
        jstr *f = argv[0].u.s;
        i = 1;
        for (uint32_t k = 0; k < f->len; k++) {
            uint16_t c = f->c[k];
            if (c == '%' && k + 1 < f->len) {
                uint16_t d = f->c[k + 1];
                if (d == '%') {
                    jsb_char(&b, '%');
                    k++;
                    continue;
                }
                if ((d == 's' || d == 'd' || d == 'i' || d == 'f' || d == 'o' || d == 'O' || d == 'c') && i < argc) {
                    jv a = argv[i++];
                    k++;
                    if (d == 'c')
                        continue;                   /* style CSS w konsoli - pomijamy */
                    if (d == 'd' || d == 'i') {
                        double x = js_to_num(rt, a);
                        jsb_num(&b, d == 'i' || d == 'd' ? js_trunc(x) : x);
                    } else if (d == 'f') {
                        jsb_num(&b, js_to_num(rt, a));
                    } else if (d == 's' && a.t == JT_STR) {
                        jsb_str(&b, a.u.s);
                    } else {
                        inspect_into(rt, &b, a, 0, d != 's');
                    }
                    continue;
                }
            }
            jsb_char(&b, c);
        }
    }
    for (; i < argc; i++) {
        if (b.len)
            jsb_char(&b, ' ');
        inspect_into(rt, &b, argv[i], 0, 0);
    }
    return jsb_done(&b);
}

static void console_out(js_rt *rt, int level, jstr *s)
{
    char buf[1024];
    js_utf8(s, buf, sizeof(buf));
    if (rt->host.console)
        rt->host.console(rt->host.ctx, level, buf);
}

static jv console_level(js_rt *rt, int argc, jv *argv, int level)
{
    console_out(rt, level, format_args(rt, argc, argv));
    return jv_undef();
}

NATIVE(con_log) { return console_level(rt, argc, argv, 0); }
NATIVE(con_warn) { return console_level(rt, argc, argv, 1); }
NATIVE(con_error) { return console_level(rt, argc, argv, 2); }

NATIVE(con_assert)
{
    if (!js_truthy(ARG(0))) {
        jstr *msg = format_args(rt, argc > 1 ? argc - 1 : 0, argv + 1);
        console_out(rt, 2, js_concat(rt, S(rt, "Assertion failed: "), msg));
    }
    return jv_undef();
}

NATIVE(con_time)
{
    jstr *label = ARG(0).t == JT_UNDEF ? js_atom(rt, "default") : js_intern(rt, js_to_str(rt, argv[0]));
    for (int i = 0; i < 16; i++)
        if (!rt->console_timers[i].label || rt->console_timers[i].label == label) {
            rt->console_timers[i].label = label;
            rt->console_timers[i].start = timer_uptime_ms();
            break;
        }
    return jv_undef();
}

NATIVE(con_time_end)
{
    jstr *label = ARG(0).t == JT_UNDEF ? js_atom(rt, "default") : js_intern(rt, js_to_str(rt, argv[0]));
    for (int i = 0; i < 16; i++)
        if (rt->console_timers[i].label == label) {
            char buf[160], name[100];
            js_utf8(label, name, sizeof(name));
            ksnprintf(buf, sizeof(buf), "%s: %u ms", name, timer_uptime_ms() - rt->console_timers[i].start);
            console_out(rt, 0, S(rt, buf));
            rt->console_timers[i].label = 0;
            break;
        }
    return jv_undef();
}

NATIVE(con_noop) { return jv_undef(); }

/* ---------- Symbol (uproszczony: symbole to teksty "@@nazwa") ---------- */

NATIVE(symbol_call)
{
    char buf[80], d[48];
    d[0] = '\0';
    if (ARG(0).t != JT_UNDEF)
        js_utf8(js_to_str(rt, argv[0]), d, sizeof(d));
    ksnprintf(buf, sizeof(buf), "@@%s#%d", d, ++rt->symbol_count);
    return jv_str(js_atom(rt, buf));
}

NATIVE(symbol_for)
{
    char buf[80], d[60];
    js_utf8(js_to_str(rt, ARG(0)), d, sizeof(d));
    ksnprintf(buf, sizeof(buf), "@@%s", d);
    return jv_str(js_atom(rt, buf));
}

/* ---------- performance, queueMicrotask, structuredClone ---------- */

NATIVE(perf_now) { return jv_num(timer_uptime_ms() - rt->clock_base_uptime); }

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

void js_lib_init(js_rt *rt)
{
    jobj *G = rt->global;
    jobj *op = rt->object_proto, *fp = rt->function_proto, *ap = rt->array_proto;

    /* globalne */
    js_define(rt, G, js_atom(rt, "NaN"), jv_num(js_nan), 0);
    js_define(rt, G, js_atom(rt, "Infinity"), jv_num(js_inf), 0);
    js_define(rt, G, rt->A.undefined, jv_undef(), 0);
    js_def_val(rt, G, "globalThis", jv_obj(G));
    js_def_fn(rt, G, "parseInt", g_parse_int, 2);
    js_def_fn(rt, G, "parseFloat", g_parse_float, 1);
    js_def_fn(rt, G, "isNaN", g_is_nan, 1);
    js_def_fn(rt, G, "isFinite", g_is_finite, 1);
    js_def_fn(rt, G, "encodeURIComponent", g_encode_uri_component, 1);
    js_def_fn(rt, G, "encodeURI", g_encode_uri, 1);
    js_def_fn(rt, G, "decodeURIComponent", g_decode_uri_component, 1);
    js_def_fn(rt, G, "decodeURI", g_decode_uri, 1);
    js_def_fn(rt, G, "escape", g_escape, 1);
    js_def_fn(rt, G, "unescape", g_unescape, 1);
    js_def_fn(rt, G, "eval", g_eval, 1);
    js_def_fn(rt, G, "btoa", g_btoa, 1);
    js_def_fn(rt, G, "atob", g_atob, 1);

    /* Object */
    jobj *O = make_ctor(rt, "Object", object_ctor, object_ctor, 1, op);
    rt->object_ctor = O;
    js_def_fn(rt, O, "keys", obj_keys, 1);
    js_def_fn(rt, O, "values", obj_values, 1);
    js_def_fn(rt, O, "entries", obj_entries, 1);
    js_def_fn(rt, O, "assign", obj_assign, 2);
    js_def_fn(rt, O, "create", obj_create, 2);
    js_def_fn(rt, O, "defineProperty", obj_define_property, 3);
    js_def_fn(rt, O, "defineProperties", obj_define_properties, 2);
    js_def_fn(rt, O, "getOwnPropertyNames", obj_get_own_property_names, 1);
    js_def_fn(rt, O, "getOwnPropertyDescriptor", obj_get_own_property_descriptor, 2);
    js_def_fn(rt, O, "getPrototypeOf", obj_get_prototype_of, 1);
    js_def_fn(rt, O, "setPrototypeOf", obj_set_prototype_of, 2);
    js_def_fn(rt, O, "freeze", obj_freeze, 1);
    js_def_fn(rt, O, "seal", obj_seal, 1);
    js_def_fn(rt, O, "preventExtensions", obj_prevent_extensions, 1);
    js_def_fn(rt, O, "isFrozen", obj_is_frozen, 1);
    js_def_fn(rt, O, "isSealed", obj_is_sealed, 1);
    js_def_fn(rt, O, "isExtensible", obj_is_extensible, 1);
    js_def_fn(rt, O, "fromEntries", obj_from_entries, 1);
    js_def_fn(rt, O, "is", obj_is, 2);
    js_def_fn(rt, O, "hasOwn", obj_has_own, 2);
    js_def_fn(rt, op, "hasOwnProperty", objp_has_own, 1);
    js_def_fn(rt, op, "isPrototypeOf", objp_is_prototype_of, 1);
    js_def_fn(rt, op, "propertyIsEnumerable", objp_property_is_enumerable, 1);
    js_def_fn(rt, op, "toString", objp_to_string, 0);
    js_def_fn(rt, op, "toLocaleString", objp_to_locale_string, 0);
    js_def_fn(rt, op, "valueOf", objp_value_of, 0);
    js_def_getset(rt, op, "__proto__", objp_proto_get, objp_proto_set);

    /* Function */
    jobj *F = make_ctor(rt, "Function", function_ctor, function_ctor, 1, fp);
    (void)F;
    js_def_fn(rt, fp, "call", fn_call, 1);
    js_def_fn(rt, fp, "apply", fn_apply, 2);
    js_def_fn(rt, fp, "bind", fn_bind, 1);
    js_def_fn(rt, fp, "toString", fn_to_string, 0);

    /* Array */
    jobj *A = make_ctor(rt, "Array", array_ctor, array_ctor, 1, ap);
    rt->array_ctor = A;
    js_def_fn(rt, A, "isArray", arr_is_array, 1);
    js_def_fn(rt, A, "of", arr_of, 0);
    js_def_fn(rt, A, "from", arr_from, 1);
    js_def_fn(rt, ap, "push", arr_push, 1);
    js_def_fn(rt, ap, "pop", arr_pop, 0);
    js_def_fn(rt, ap, "shift", arr_shift, 0);
    js_def_fn(rt, ap, "unshift", arr_unshift, 1);
    js_def_fn(rt, ap, "slice", arr_slice, 2);
    js_def_fn(rt, ap, "splice", arr_splice, 2);
    js_def_fn(rt, ap, "concat", arr_concat, 1);
    js_def_fn(rt, ap, "join", arr_join, 1);
    js_def_fn(rt, ap, "toString", arr_to_string, 0);
    js_def_fn(rt, ap, "toLocaleString", arr_to_string, 0);
    js_def_fn(rt, ap, "reverse", arr_reverse, 0);
    js_def_fn(rt, ap, "indexOf", arr_index_of, 1);
    js_def_fn(rt, ap, "lastIndexOf", arr_last_index_of, 1);
    js_def_fn(rt, ap, "includes", arr_includes, 1);
    js_def_fn(rt, ap, "forEach", arr_for_each, 1);
    js_def_fn(rt, ap, "map", arr_map, 1);
    js_def_fn(rt, ap, "filter", arr_filter, 1);
    js_def_fn(rt, ap, "some", arr_some, 1);
    js_def_fn(rt, ap, "every", arr_every, 1);
    js_def_fn(rt, ap, "find", arr_find, 1);
    js_def_fn(rt, ap, "findIndex", arr_find_index, 1);
    js_def_fn(rt, ap, "findLast", arr_find_last, 1);
    js_def_fn(rt, ap, "findLastIndex", arr_find_last_index, 1);
    js_def_fn(rt, ap, "reduce", arr_reduce, 1);
    js_def_fn(rt, ap, "reduceRight", arr_reduce_right, 1);
    js_def_fn(rt, ap, "sort", arr_sort, 1);
    js_def_fn(rt, ap, "fill", arr_fill, 1);
    js_def_fn(rt, ap, "keys", arr_keys, 0);
    js_def_fn(rt, ap, "values", arr_values, 0);
    js_def_fn(rt, ap, "entries", arr_entries, 0);
    js_def_fn(rt, ap, "flat", arr_flat, 0);
    js_def_fn(rt, ap, "flatMap", arr_flat_map, 1);
    js_def_fn(rt, ap, "at", arr_at, 1);
    js_def_fn(rt, ap, "copyWithin", arr_copy_within, 2);
    js_define(rt, ap, rt->sym_iterator, js_get(rt, jv_obj(ap), js_atom(rt, "values")), PF_WRITE | PF_CONF);

    /* Error i pochodne */
    static const char *const names[] = { "Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError", "EvalError", "URIError" };
    for (int k = 0; k < 7; k++) {
        jobj *proto = k == 0 ? rt->error_proto : js_new_object(rt, rt->error_proto);
        rt->error_protos[k] = proto;
        jobj *E = make_ctor(rt, names[k], error_ctor, error_ctor, 1, proto);
        E->u.nf.data = jv_num(k);
        if (k)
            E->proto = rt->error_ctors[0];
        rt->error_ctors[k] = E;
        js_def_val(rt, proto, "name", jv_str(js_atom(rt, names[k])));
        js_def_val(rt, proto, "message", jv_str(rt->empty));
    }
    js_def_fn(rt, rt->error_proto, "toString", errp_to_string, 0);
    js_def_fn(rt, rt->error_ctors[0], "captureStackTrace", err_capture_stack, 1);

    /* Boolean */
    make_ctor(rt, "Boolean", boolean_call, boolean_ctor, 1, rt->boolean_proto);
    rt->boolean_proto->cls = JC_BOOL;
    rt->boolean_proto->u.prim = jv_bool(0);
    js_def_fn(rt, rt->boolean_proto, "toString", boolp_to_string, 0);
    js_def_fn(rt, rt->boolean_proto, "valueOf", boolp_value_of, 0);

    /* Number */
    jobj *np = rt->number_proto;
    jobj *N = make_ctor(rt, "Number", number_call, number_ctor, 1, np);
    np->cls = JC_NUM;
    np->u.prim = jv_num(0);
    js_def_fn(rt, np, "toString", nump_to_string, 1);
    js_def_fn(rt, np, "toFixed", nump_to_fixed, 1);
    js_def_fn(rt, np, "toPrecision", nump_to_precision, 1);
    js_def_fn(rt, np, "toExponential", nump_to_exponential, 1);
    js_def_fn(rt, np, "toLocaleString", nump_to_locale_string, 0);
    js_def_fn(rt, np, "valueOf", nump_value_of, 0);
    js_def_fn(rt, N, "isInteger", num_is_integer, 1);
    js_def_fn(rt, N, "isSafeInteger", num_is_safe_integer, 1);
    js_def_fn(rt, N, "isFinite", num_is_finite, 1);
    js_def_fn(rt, N, "isNaN", num_is_nan, 1);
    js_def_val(rt, N, "parseFloat", js_get(rt, jv_obj(G), js_atom(rt, "parseFloat")));
    js_def_val(rt, N, "parseInt", js_get(rt, jv_obj(G), js_atom(rt, "parseInt")));
    js_define(rt, N, js_atom(rt, "MAX_SAFE_INTEGER"), jv_num(9007199254740991.0), 0);
    js_define(rt, N, js_atom(rt, "MIN_SAFE_INTEGER"), jv_num(-9007199254740991.0), 0);
    js_define(rt, N, js_atom(rt, "MAX_VALUE"), jv_num(1.7976931348623157e308), 0);
    js_define(rt, N, js_atom(rt, "MIN_VALUE"), jv_num(5e-324), 0);
    js_define(rt, N, js_atom(rt, "EPSILON"), jv_num(2.220446049250313e-16), 0);
    js_define(rt, N, js_atom(rt, "POSITIVE_INFINITY"), jv_num(js_inf), 0);
    js_define(rt, N, js_atom(rt, "NEGATIVE_INFINITY"), jv_num(-js_inf), 0);
    js_define(rt, N, js_atom(rt, "NaN"), jv_num(js_nan), 0);

    /* Math */
    jobj *M = js_new_object(rt, op);
    js_def_val(rt, G, "Math", jv_obj(M));
    static const struct { const char *n; jnative f; int len; } mf[] = {
        { "abs", m_abs, 1 }, { "floor", m_floor, 1 }, { "ceil", m_ceil, 1 }, { "round", m_round, 1 },
        { "trunc", m_trunc, 1 }, { "sign", m_sign, 1 }, { "sqrt", m_sqrt, 1 }, { "cbrt", m_cbrt, 1 },
        { "exp", m_exp, 1 }, { "expm1", m_expm1, 1 }, { "log", m_log, 1 }, { "log1p", m_log1p, 1 },
        { "log10", m_log10, 1 }, { "log2", m_log2, 1 }, { "sin", m_sin, 1 }, { "cos", m_cos, 1 },
        { "tan", m_tan, 1 }, { "asin", m_asin, 1 }, { "acos", m_acos, 1 }, { "atan", m_atan, 1 },
        { "sinh", m_sinh, 1 }, { "cosh", m_cosh, 1 }, { "tanh", m_tanh, 1 }, { "asinh", m_asinh, 1 },
        { "acosh", m_acosh, 1 }, { "atanh", m_atanh, 1 }, { "fround", m_fround, 1 }, { "pow", m_pow, 2 },
        { "atan2", m_atan2, 2 }, { "min", m_min, 2 }, { "max", m_max, 2 }, { "hypot", m_hypot, 2 },
        { "random", m_random, 0 }, { "clz32", m_clz32, 1 }, { "imul", m_imul, 2 },
    };
    for (unsigned i = 0; i < sizeof(mf) / sizeof(mf[0]); i++)
        js_def_fn(rt, M, mf[i].n, mf[i].f, mf[i].len);
    js_define(rt, M, js_atom(rt, "PI"), jv_num(3.141592653589793), 0);
    js_define(rt, M, js_atom(rt, "E"), jv_num(2.718281828459045), 0);
    js_define(rt, M, js_atom(rt, "LN2"), jv_num(0.6931471805599453), 0);
    js_define(rt, M, js_atom(rt, "LN10"), jv_num(2.302585092994046), 0);
    js_define(rt, M, js_atom(rt, "LOG2E"), jv_num(1.4426950408889634), 0);
    js_define(rt, M, js_atom(rt, "LOG10E"), jv_num(0.4342944819032518), 0);
    js_define(rt, M, js_atom(rt, "SQRT2"), jv_num(1.4142135623730951), 0);
    js_define(rt, M, js_atom(rt, "SQRT1_2"), jv_num(0.7071067811865476), 0);

    /* JSON */
    jobj *J = js_new_object(rt, op);
    js_def_val(rt, G, "JSON", jv_obj(J));
    js_def_fn(rt, J, "parse", json_parse, 2);
    js_def_fn(rt, J, "stringify", json_stringify, 3);

    /* console */
    jobj *C = js_new_object(rt, op);
    js_def_val(rt, G, "console", jv_obj(C));
    js_def_fn(rt, C, "log", con_log, 0);
    js_def_fn(rt, C, "info", con_log, 0);
    js_def_fn(rt, C, "debug", con_log, 0);
    js_def_fn(rt, C, "dir", con_log, 0);
    js_def_fn(rt, C, "table", con_log, 0);
    js_def_fn(rt, C, "trace", con_log, 0);
    js_def_fn(rt, C, "warn", con_warn, 0);
    js_def_fn(rt, C, "error", con_error, 0);
    js_def_fn(rt, C, "assert", con_assert, 0);
    js_def_fn(rt, C, "time", con_time, 0);
    js_def_fn(rt, C, "timeEnd", con_time_end, 0);
    js_def_fn(rt, C, "group", con_log, 0);
    js_def_fn(rt, C, "groupCollapsed", con_log, 0);
    js_def_fn(rt, C, "groupEnd", con_noop, 0);
    js_def_fn(rt, C, "clear", con_noop, 0);
    js_def_fn(rt, C, "count", con_log, 0);

    /* Symbol */
    jobj *Sy = js_new_native(rt, "Symbol", symbol_call, 0);
    js_def_val(rt, G, "Symbol", jv_obj(Sy));
    js_def_fn(rt, Sy, "for", symbol_for, 1);
    js_define(rt, Sy, js_atom(rt, "iterator"), jv_str(rt->sym_iterator), 0);
    js_define(rt, Sy, js_atom(rt, "asyncIterator"), jv_str(js_atom(rt, "@@asyncIterator")), 0);
    js_define(rt, Sy, js_atom(rt, "hasInstance"), jv_str(js_atom(rt, "@@hasInstance")), 0);
    js_define(rt, Sy, js_atom(rt, "toPrimitive"), jv_str(js_atom(rt, "@@toPrimitive")), 0);
    js_define(rt, Sy, js_atom(rt, "toStringTag"), jv_str(js_atom(rt, "@@toStringTag")), 0);

    /* performance */
    jobj *P = js_new_object(rt, op);
    js_def_val(rt, G, "performance", jv_obj(P));
    js_def_fn(rt, P, "now", perf_now, 0);

    js_lib2_init(rt);
}

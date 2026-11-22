/* fetch i XMLHttpRequest - pobieranie danych przez skrypt strony.
 *
 * Skrypt nie moze czekac na siec (zamarloby cale okno), wiec:
 *   1. fetch(url) tylko zapisuje zapytanie w rt->net i od razu zwraca Promise,
 *   2. przegladarka odbiera zapytanie (js_take_request) i pobiera je w osobnym
 *      watku (http_request),
 *   3. gdy odpowiedz przyjdzie, przegladarka wola js_request_done - Promise
 *      dostaje obiekt Response, a XMLHttpRequest - zdarzenia load/readystatechange.
 *
 * Tak samo w tools/jstest (tam "siec" to pliki obok strony testowej). */

#include "js_int.h"
#include "string.h"
#include "heap.h"
#include "http.h"

#define U __attribute__((unused))
#define NATIVE(name) static jv name(js_rt *rt U, jv self U, int argc U, jv *argv U, jobj *fn U)
#define ARG(i) ((i) < argc ? argv[i] : jv_undef())
#define BUDGET_EVENT 3000

static jv str8(js_rt *rt, const char *s) { return jv_str(js_str_utf8(rt, s, (uint32_t)strlen(s))); }

static char *arena_copy(js_rt *rt, const char *s, unsigned len)
{
    char *p = js_alloc(rt, len + 1);
    memcpy(p, s, len);
    p[len] = 0;
    return p;
}

static char *kcopy(const char *s, unsigned len)
{
    if (!s)
        return 0;
    char *p = kmalloc(len + 1);
    if (p) {
        memcpy(p, s, len);
        p[len] = 0;
    }
    return p;
}

/* Wlasciwosc ukryta (nie widac jej w for-in) */
static void hidden(js_rt *rt, jobj *o, const char *name, jv v)
{
    jstr *k = js_atom(rt, name);
    struct jprop *p = js_own(o, k);
    if (p)
        p->v = v;
    else
        js_define(rt, o, k, v, PF_WRITE);
}

static jv hidden_get(js_rt *rt, jv o, const char *name)
{
    if (o.t != JT_OBJ)
        return jv_undef();
    struct jprop *p = js_own(o.u.o, js_atom(rt, name));
    return p ? p->v : jv_undef();
}

static const char *status_text(int s)
{
    switch (s) {
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    }
    return "";
}

/* ---------- kolejka zapytan ---------- */

static int add_request(js_rt *rt, int xhr, jobj *target, const char *method, const char *url,
                       const char *headers, unsigned hlen, const char *body, unsigned blen)
{
    struct js_netreq *q = 0;
    for (int i = 0; i < JS_NET_MAX; i++)
        if (!rt->net[i].state) {
            q = &rt->net[i];
            break;
        }
    if (!q)
        return 0;
    q->id = ++rt->net_next_id;
    q->state = 1;
    q->xhr = (uint8_t)xhr;
    q->target = target;
    ksnprintf(q->method, sizeof(q->method), "%s", method);
    q->url = arena_copy(rt, url, (unsigned)strlen(url));
    q->headers = hlen ? arena_copy(rt, headers, hlen) : 0;
    q->body = body ? arena_copy(rt, body, blen) : 0;
    q->body_len = body ? blen : 0;
    return q->id;
}

int js_net_pending(js_rt *rt)
{
    int n = 0;
    for (int i = 0; rt && i < JS_NET_MAX; i++)
        if (rt->net[i].state)
            n++;
    return n;
}

int js_take_request(struct js_rt *rt, struct js_request *out)
{
    if (!rt || rt->dead)
        return 0;
    struct js_netreq *q = 0;
    for (int i = 0; i < JS_NET_MAX; i++)                 /* najstarsze pierwsze */
        if (rt->net[i].state == 1 && (!q || rt->net[i].id < q->id))
            q = &rt->net[i];
    if (!q)
        return 0;
    q->state = 2;
    memset(out, 0, sizeof(*out));
    out->id = q->id;
    ksnprintf(out->method, sizeof(out->method), "%s", q->method);
    ksnprintf(out->url, sizeof(out->url), "%s", q->url);
    out->headers = q->headers ? kcopy(q->headers, (unsigned)strlen(q->headers)) : 0;
    out->body = q->body ? kcopy(q->body, q->body_len) : 0;
    out->body_len = q->body ? q->body_len : 0;
    return 1;
}

/* Metoda wielkimi literami; tylko litery (bez dziwnych znakow w naglowku HTTP) */
static int clean_method(js_rt *rt, jv v, char *out, unsigned size)
{
    if (v.t == JT_UNDEF) {
        ksnprintf(out, size, "GET");
        return 1;
    }
    char buf[16];
    js_utf8(js_to_str(rt, v), buf, sizeof(buf));
    unsigned n = 0;
    for (const char *s = buf; *s && n + 1 < size; s++) {
        char c = *s >= 'a' && *s <= 'z' ? (char)(*s - 32) : *s;
        if (c < 'A' || c > 'Z')
            return 0;
        out[n++] = c;
    }
    out[n] = 0;
    return n > 0;
}

/* Naglowek do tekstu zapytania: bez znakow nowej linii (wstrzykniecie naglowkow) */
static void add_header(struct jsb *b, js_rt *rt, jv name, jv value)
{
    char n[80], v[600];
    js_utf8(js_to_str(rt, name), n, sizeof(n));
    js_utf8(js_to_str(rt, value), v, sizeof(v));
    for (char *p = n; *p; p++)
        if (*p == '\r' || *p == '\n' || *p == ':')
            return;
    for (char *p = v; *p; p++)
        if (*p == '\r' || *p == '\n')
            *p = ' ';
    if (!n[0])
        return;
    jsb_utf8(b, n, (unsigned)strlen(n));
    jsb_ascii(b, ": ");
    jsb_utf8(b, v, (unsigned)strlen(v));
    jsb_ascii(b, "\r\n");
}

static int has_header(js_rt *rt, jstr *headers, const char *name)
{
    if (!headers || !headers->len)
        return 0;
    char *s = js_utf8_alloc(rt, headers, 0);
    char pat[40];
    ksnprintf(pat, sizeof(pat), "%s:", name);
    if (str_starts_ci(s, pat))
        return 1;
    ksnprintf(pat, sizeof(pat), "\n%s:", name);
    return strstr_ci(s, pat) != 0;
}

/* init.headers: zwykly obiekt { "Content-Type": "..." }, tablica par albo Headers */
static jstr *headers_from(js_rt *rt, jv h)
{
    struct jsb b;
    jsb_init(&b, rt);
    if (h.t == JT_OBJ) {
        jv raw = hidden_get(rt, h, "__sent");             /* obiekt Headers - jego wlasne pary */
        if (raw.t == JT_OBJ)
            h = raw;
        if (h.u.o->cls == JC_ARRAY) {
            for (uint32_t i = 0; i < h.u.o->u.arr.len; i++) {
                jv pair = h.u.o->u.arr.v[i];
                if (pair.t == JT_OBJ && pair.u.o->cls == JC_ARRAY && pair.u.o->u.arr.len >= 2)
                    add_header(&b, rt, pair.u.o->u.arr.v[0], pair.u.o->u.arr.v[1]);
            }
        } else {
            jobj *keys = js_own_keys(rt, h.u.o, 1);
            for (uint32_t i = 0; i < keys->u.arr.len; i++) {
                jv k = keys->u.arr.v[i];
                add_header(&b, rt, k, js_get(rt, h, js_key_of(rt, k)));
            }
        }
    }
    return jsb_done(&b);
}

/* Adres wzgledem strony. 0 = zly adres albo nie http(s) */
static int abs_url(js_rt *rt, jv v, char *out, unsigned size)
{
    char s[URL_MAX];
    js_utf8(js_to_str(rt, v), s, sizeof(s));
    if (!s[0] && rt->url[0]) {
        ksnprintf(out, size, "%s", rt->url);
        return 1;
    }
    if (!url_resolve(rt->url[0] ? rt->url : "http://localhost/", s, out, size))
        return 0;
    return str_starts_ci(out, "http://") || str_starts_ci(out, "https://");
}

/* ---------- fetch ---------- */

static jv reject_now(js_rt *rt, const char *msg)
{
    jobj *p = js_new_promise(rt);
    js_promise_reject(rt, p, jv_obj(js_make_error(rt, JS_ERR_TYPE, js_str_c(rt, msg))));
    return jv_obj(p);
}

NATIVE(w_fetch)
{
    jv input = ARG(0), init = ARG(1);
    if (input.t == JT_OBJ) {                             /* fetch(new Request(...)) / fetch(URL) */
        jv u = js_get(rt, input, js_atom(rt, "url"));
        if (u.t == JT_STR)
            input = u;
        else
            input = jv_str(js_to_str(rt, input));
    }
    char url[URL_MAX];
    if (!abs_url(rt, input, url, sizeof(url)))
        return reject_now(rt, "Failed to fetch (NaszOS: zly adres albo nie http/https)");
    char method[12];
    jv body = jv_undef(), hdr = jv_undef();
    if (init.t == JT_OBJ) {
        if (!clean_method(rt, js_get(rt, init, js_atom(rt, "method")), method, sizeof(method)))
            js_throw_error(rt, JS_ERR_TYPE, "Failed to execute 'fetch': invalid method");
        body = js_get(rt, init, js_atom(rt, "body"));
        hdr = js_get(rt, init, js_atom(rt, "headers"));
    } else {
        ksnprintf(method, sizeof(method), "GET");
    }
    if (!IS_NULLISH(body) && (strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0))
        js_throw_error(rt, JS_ERR_TYPE, "Failed to execute 'fetch': Request with GET/HEAD method cannot have body.");
    jstr *headers = headers_from(rt, hdr);
    char *b = 0;
    unsigned blen = 0;
    if (!IS_NULLISH(body)) {
        b = js_utf8_alloc(rt, js_to_str(rt, body), &blen);
        if (!has_header(rt, headers, "content-type")) {  /* jak przegladarki: tekst = text/plain */
            struct jsb sb;
            jsb_init(&sb, rt);
            jsb_str(&sb, headers);
            jsb_ascii(&sb, "Content-Type: text/plain;charset=UTF-8\r\n");
            headers = jsb_done(&sb);
        }
    }
    unsigned hlen;
    char *h = js_utf8_alloc(rt, headers, &hlen);
    jobj *p = js_new_promise(rt);
    if (!add_request(rt, 0, p, method, url, h, hlen, b, blen))
        js_promise_reject(rt, p, jv_obj(js_make_error(rt, JS_ERR_TYPE, js_str_c(rt, "Failed to fetch (za duzo zapytan naraz)"))));
    return jv_obj(p);
}

/* ---------- Headers (odpowiedzi) ---------- */

/* Naglowki odpowiedzi trzymamy jako tekst "__raw"; get() szuka w nim linii */
static int header_value(js_rt *rt, jv self, jv name, struct jsb *out)
{
    jv raw = hidden_get(rt, self, "__raw");
    if (raw.t != JT_STR)
        return 0;
    char n[80];
    js_utf8(js_to_str(rt, name), n, sizeof(n));
    unsigned nl = (unsigned)strlen(n);
    char *s = js_utf8_alloc(rt, raw.u.s, 0);
    int found = 0;
    const char *line = s;
    while (*line && *line != '\n')                       /* pierwsza linia: HTTP/1.1 200 OK */
        line++;
    while (*line) {
        if (*line == '\n')
            line++;
        if (nl && str_starts_ci(line, n) && line[nl] == ':') {
            const char *v = line + nl + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            const char *e = v;
            while (*e && *e != '\r' && *e != '\n')
                e++;
            if (found && out)
                jsb_ascii(out, ", ");
            if (out)
                jsb_utf8(out, v, (unsigned)(e - v));
            found = 1;
        }
        while (*line && *line != '\n')
            line++;
    }
    return found;
}

NATIVE(h_get)
{
    struct jsb b;
    jsb_init(&b, rt);
    return header_value(rt, self, ARG(0), &b) ? jv_str(jsb_done(&b)) : jv_null();
}

NATIVE(h_has) { return jv_bool(header_value(rt, self, ARG(0), 0)); }

/* Lista par [nazwa malymi literami, wartosc] - dla forEach i entries */
static jobj *header_pairs(js_rt *rt, jv self)
{
    jobj *a = js_new_array(rt, 0);
    jv raw = hidden_get(rt, self, "__raw");
    if (raw.t != JT_STR)
        return a;
    char *s = js_utf8_alloc(rt, raw.u.s, 0);
    const char *line = s;
    while (*line && *line != '\n')
        line++;
    while (*line) {
        if (*line == '\n')
            line++;
        const char *colon = line;
        while (*colon && *colon != ':' && *colon != '\n')
            colon++;
        if (*colon == ':' && colon > line) {
            char name[80];
            unsigned l = (unsigned)(colon - line) < sizeof(name) - 1 ? (unsigned)(colon - line) : sizeof(name) - 1;
            for (unsigned i = 0; i < l; i++)
                name[i] = line[i] >= 'A' && line[i] <= 'Z' ? (char)(line[i] + 32) : line[i];
            name[l] = 0;
            const char *v = colon + 1;
            while (*v == ' ' || *v == '\t')
                v++;
            const char *e = v;
            while (*e && *e != '\r' && *e != '\n')
                e++;
            jobj *pair = js_new_array(rt, 0);
            js_array_push(rt, pair, str8(rt, name));
            js_array_push(rt, pair, jv_str(js_str_utf8(rt, v, (uint32_t)(e - v))));
            js_array_push(rt, a, jv_obj(pair));
        }
        while (*line && *line != '\n')
            line++;
    }
    return a;
}

NATIVE(h_entries) { return jv_obj(header_pairs(rt, self)); }

NATIVE(h_for_each)
{
    jv f = ARG(0);
    if (!js_is_callable(f))
        js_throw_error(rt, JS_ERR_TYPE, "Headers.forEach: callback is not a function");
    jobj *a = header_pairs(rt, self);
    for (uint32_t i = 0; i < a->u.arr.len; i++) {
        jobj *pair = a->u.arr.v[i].u.o;
        jv args[3] = { pair->u.arr.v[1], pair->u.arr.v[0], self };
        js_call(rt, f, ARG(1), 3, args);
    }
    return jv_undef();
}

static jv new_headers(js_rt *rt, const char *raw)
{
    jobj *h = js_new_object(rt, rt->headers_proto);
    hidden(rt, h, "__raw", raw ? str8(rt, raw) : jv_str(rt->empty));
    return jv_obj(h);
}

/* ---------- Response ---------- */

static jv body_text(js_rt *rt, jv self)
{
    jv b = hidden_get(rt, self, "__body");
    if (b.t != JT_STR)
        js_throw_error(rt, JS_ERR_TYPE, "Illegal invocation");
    if (js_truthy(hidden_get(rt, self, "__used")))
        js_throw_error(rt, JS_ERR_TYPE, "Failed to execute 'text' on 'Response': body stream already read");
    hidden(rt, self.u.o, "__used", jv_bool(1));
    return b;
}

NATIVE(r_text)
{
    jobj *p = js_new_promise(rt);
    js_promise_resolve(rt, p, body_text(rt, self));
    return jv_obj(p);
}

NATIVE(r_json)
{
    jobj *p = js_new_promise(rt);
    jv text = body_text(rt, self);
    jv json = js_get(rt, jv_obj(rt->global), js_atom(rt, "JSON"));
    jv parse = js_get(rt, json, js_atom(rt, "parse"));
    jv r;
    if (js_try_call(rt, parse, json, 1, &text, &r))
        js_promise_reject(rt, p, r);                     /* zly JSON - SyntaxError w Promise */
    else
        js_promise_resolve(rt, p, r);
    return jv_obj(p);
}

/* arrayBuffer / blob - nie mamy tych typow; dajemy tablice bajtow / tekst */
NATIVE(r_array_buffer)
{
    jobj *p = js_new_promise(rt);
    jv text = body_text(rt, self);
    unsigned len;
    char *s = js_utf8_alloc(rt, text.u.s, &len);
    jobj *a = js_new_array(rt, 0);
    js_array_reserve(rt, a, len ? len : 1);
    for (unsigned i = 0; i < len; i++)
        a->u.arr.v[i] = jv_num((uint8_t)s[i]);
    a->u.arr.len = len;
    js_promise_resolve(rt, p, jv_obj(a));
    return jv_obj(p);
}

NATIVE(r_clone)
{
    if (self.t != JT_OBJ)
        js_throw_error(rt, JS_ERR_TYPE, "Illegal invocation");
    jobj *o = js_new_object(rt, rt->response_proto);
    jobj *keys = js_own_keys(rt, self.u.o, 0);
    for (uint32_t i = 0; i < keys->u.arr.len; i++) {
        jstr *k = js_key_of(rt, keys->u.arr.v[i]);
        struct jprop *pr = js_own(self.u.o, k);
        if (pr)
            js_define(rt, o, k, pr->v, pr->flags);
    }
    hidden(rt, o, "__used", jv_bool(0));
    return jv_obj(o);
}

static jv new_response(js_rt *rt, int status, const char *headers, const char *body, unsigned len, const char *url)
{
    jobj *o = js_new_object(rt, rt->response_proto);
    jv ov = jv_obj(o);
    js_set(rt, ov, rt->A.status, jv_num(status));
    js_set(rt, ov, js_atom(rt, "statusText"), str8(rt, status_text(status)));
    js_set(rt, ov, js_atom(rt, "ok"), jv_bool(status >= 200 && status < 300));
    js_set(rt, ov, js_atom(rt, "url"), str8(rt, url ? url : ""));
    js_set(rt, ov, js_atom(rt, "redirected"), jv_bool(0));
    js_set(rt, ov, rt->A.type, str8(rt, "basic"));
    js_set(rt, ov, js_atom(rt, "headers"), new_headers(rt, headers));
    js_set(rt, ov, js_atom(rt, "bodyUsed"), jv_bool(0));
    hidden(rt, o, "__body", jv_str(js_str_utf8(rt, body ? body : "", body ? len : 0)));
    hidden(rt, o, "__used", jv_bool(0));
    return ov;
}

/* ---------- XMLHttpRequest ---------- */

static jobj *XHR(js_rt *rt, jv self)
{
    if (self.t != JT_OBJ || hidden_get(rt, self, "__xhr").t != JT_BOOL)
        js_throw_error(rt, JS_ERR_TYPE, "Illegal invocation");
    return self.u.o;
}

/* Zdarzenie: najpierw xhr.onX, potem addEventListener */
static void xhr_fire(js_rt *rt, jobj *x, const char *type)
{
    jobj *ev = js_new_object(rt, rt->object_proto);
    jv evv = jv_obj(ev);
    js_set(rt, evv, rt->A.type, str8(rt, type));
    js_set(rt, evv, rt->A.target, jv_obj(x));
    js_set(rt, evv, rt->A.currentTarget, jv_obj(x));
    char name[40];
    ksnprintf(name, sizeof(name), "on%s", type);
    jv h = js_get(rt, jv_obj(x), js_atom(rt, name)), r;
    if (js_is_callable(h) && js_try_call(rt, h, jv_obj(x), 1, &evv, &r)) {
        rt->exc = r;
        js_report_exception(rt);
    }
    ksnprintf(name, sizeof(name), "__ev_%s", type);
    jv list = hidden_get(rt, jv_obj(x), name);
    if (list.t == JT_OBJ && list.u.o->cls == JC_ARRAY)
        for (uint32_t i = 0; i < list.u.o->u.arr.len; i++) {
            jv f = list.u.o->u.arr.v[i];
            if (js_is_callable(f) && js_try_call(rt, f, jv_obj(x), 1, &evv, &r)) {
                rt->exc = r;
                js_report_exception(rt);
            }
        }
}

static void xhr_state(js_rt *rt, jobj *x, int state)
{
    js_set(rt, jv_obj(x), js_atom(rt, "readyState"), jv_num(state));
    xhr_fire(rt, x, "readystatechange");
}

NATIVE(x_open)
{
    jobj *x = XHR(rt, self);
    char method[12], url[URL_MAX];
    if (!clean_method(rt, ARG(0), method, sizeof(method)))
        js_throw_error(rt, JS_ERR_SYNTAX, "Failed to execute 'open' on 'XMLHttpRequest': invalid method");
    if (!abs_url(rt, ARG(1), url, sizeof(url)))
        js_throw_error(rt, JS_ERR_SYNTAX, "Failed to execute 'open' on 'XMLHttpRequest': Invalid URL");
    hidden(rt, x, "__method", str8(rt, method));
    hidden(rt, x, "__url", str8(rt, url));
    hidden(rt, x, "__hdr", jv_str(rt->empty));
    hidden(rt, x, "__id", jv_num(0));
    js_set(rt, self, rt->A.status, jv_num(0));
    js_set(rt, self, js_atom(rt, "responseText"), jv_str(rt->empty));
    js_set(rt, self, js_atom(rt, "response"), jv_str(rt->empty));
    xhr_state(rt, x, 1);
    return jv_undef();
}

NATIVE(x_set_request_header)
{
    jobj *x = XHR(rt, self);
    struct jsb b;
    jsb_init(&b, rt);
    jv old = hidden_get(rt, self, "__hdr");
    if (old.t == JT_STR)
        jsb_str(&b, old.u.s);
    add_header(&b, rt, ARG(0), ARG(1));
    hidden(rt, x, "__hdr", jv_str(jsb_done(&b)));
    return jv_undef();
}

NATIVE(x_send)
{
    jobj *x = XHR(rt, self);
    jv m = hidden_get(rt, self, "__method"), u = hidden_get(rt, self, "__url");
    if (m.t != JT_STR || u.t != JT_STR)
        js_throw_error(rt, JS_ERR_ERROR, "Failed to execute 'send' on 'XMLHttpRequest': The object's state must be OPENED.");
    char method[12], url[URL_MAX];
    js_utf8(m.u.s, method, sizeof(method));
    js_utf8(u.u.s, url, sizeof(url));
    jv hv = hidden_get(rt, self, "__hdr");
    jstr *headers = hv.t == JT_STR ? hv.u.s : rt->empty;
    char *b = 0;
    unsigned blen = 0;
    if (!IS_NULLISH(ARG(0)) && strcmp(method, "GET") && strcmp(method, "HEAD")) {
        b = js_utf8_alloc(rt, js_to_str(rt, argv[0]), &blen);
        if (!has_header(rt, headers, "content-type")) {
            struct jsb sb;
            jsb_init(&sb, rt);
            jsb_str(&sb, headers);
            jsb_ascii(&sb, "Content-Type: text/plain;charset=UTF-8\r\n");
            headers = jsb_done(&sb);
        }
    }
    unsigned hlen;
    char *h = js_utf8_alloc(rt, headers, &hlen);
    int id = add_request(rt, 1, x, method, url, h, hlen, b, blen);
    hidden(rt, x, "__id", jv_num(id));
    xhr_fire(rt, x, "loadstart");
    if (!id) {                                           /* za duzo zapytan naraz */
        xhr_state(rt, x, 4);
        xhr_fire(rt, x, "error");
        xhr_fire(rt, x, "loadend");
    }
    return jv_undef();
}

NATIVE(x_abort)
{
    jobj *x = XHR(rt, self);
    int id = (int)js_to_num(rt, hidden_get(rt, self, "__id"));
    for (int i = 0; i < JS_NET_MAX; i++)
        if (rt->net[i].state && rt->net[i].id == id) {
            rt->net[i].state = 0;                        /* odpowiedz (jesli przyjdzie) zignorujemy */
            hidden(rt, x, "__id", jv_num(0));
            xhr_state(rt, x, 4);
            xhr_fire(rt, x, "abort");
            xhr_fire(rt, x, "loadend");
            js_set(rt, self, js_atom(rt, "readyState"), jv_num(0));
            break;
        }
    return jv_undef();
}

NATIVE(x_get_response_header)
{
    XHR(rt, self);
    jv h = hidden_get(rt, self, "__headers");
    if (h.t != JT_OBJ)
        return jv_null();
    struct jsb b;
    jsb_init(&b, rt);
    return header_value(rt, h, ARG(0), &b) ? jv_str(jsb_done(&b)) : jv_null();
}

NATIVE(x_get_all_response_headers)
{
    XHR(rt, self);
    jv h = hidden_get(rt, self, "__headers");
    struct jsb b;
    jsb_init(&b, rt);
    if (h.t == JT_OBJ) {
        jobj *a = header_pairs(rt, h);
        for (uint32_t i = 0; i < a->u.arr.len; i++) {
            jobj *pair = a->u.arr.v[i].u.o;
            jsb_str(&b, js_to_str(rt, pair->u.arr.v[0]));
            jsb_ascii(&b, ": ");
            jsb_str(&b, js_to_str(rt, pair->u.arr.v[1]));
            jsb_ascii(&b, "\r\n");
        }
    }
    return jv_str(jsb_done(&b));
}

NATIVE(x_add_event_listener)
{
    jobj *x = XHR(rt, self);
    if (!js_is_callable(ARG(1)))
        return jv_undef();
    char type[32], name[48];
    js_utf8(js_to_str(rt, ARG(0)), type, sizeof(type));
    ksnprintf(name, sizeof(name), "__ev_%s", type);
    jv list = hidden_get(rt, self, name);
    if (list.t != JT_OBJ) {
        list = jv_obj(js_new_array(rt, 0));
        hidden(rt, x, name, list);
    }
    js_array_push(rt, list.u.o, argv[1]);
    return jv_undef();
}

NATIVE(x_remove_event_listener)
{
    XHR(rt, self);
    char type[32], name[48];
    js_utf8(js_to_str(rt, ARG(0)), type, sizeof(type));
    ksnprintf(name, sizeof(name), "__ev_%s", type);
    jv list = hidden_get(rt, self, name);
    if (list.t == JT_OBJ && list.u.o->cls == JC_ARRAY)
        for (uint32_t i = 0; i < list.u.o->u.arr.len; i++)
            if (js_strict_eq(list.u.o->u.arr.v[i], ARG(1)))
                list.u.o->u.arr.v[i] = jv_undef();
    return jv_undef();
}

NATIVE(x_noop) { return jv_undef(); }

static jv xhr_call(js_rt *rt, jv self U, int argc U, jv *argv U, jobj *fn U)
{
    js_throw_error(rt, JS_ERR_TYPE, "Failed to construct 'XMLHttpRequest': Please use the 'new' operator.");
}

static jv xhr_ctor(js_rt *rt, jv self U, int argc U, jv *argv U, jobj *fn U)
{
    jobj *o = js_new_object(rt, rt->xhr_proto);
    hidden(rt, o, "__xhr", jv_bool(1));
    jv ov = jv_obj(o);
    js_set(rt, ov, js_atom(rt, "readyState"), jv_num(0));
    js_set(rt, ov, rt->A.status, jv_num(0));
    js_set(rt, ov, js_atom(rt, "statusText"), jv_str(rt->empty));
    js_set(rt, ov, js_atom(rt, "responseText"), jv_str(rt->empty));
    js_set(rt, ov, js_atom(rt, "response"), jv_str(rt->empty));
    js_set(rt, ov, js_atom(rt, "responseType"), jv_str(rt->empty));
    js_set(rt, ov, js_atom(rt, "responseURL"), jv_str(rt->empty));
    js_set(rt, ov, js_atom(rt, "timeout"), jv_num(0));
    js_set(rt, ov, js_atom(rt, "withCredentials"), jv_bool(0));
    return ov;
}

/* ---------- odpowiedz z przegladarki ---------- */

struct done_args {
    struct js_netreq q;
    int status;
    const char *headers, *body, *url;
    unsigned len;
};

static void done_cb(js_rt *rt, void *arg)
{
    struct done_args *a = arg;
    if (!a->q.xhr) {
        if (a->status <= 0)
            js_promise_reject(rt, a->q.target, jv_obj(js_make_error(rt, JS_ERR_TYPE, js_str_c(rt, "Failed to fetch"))));
        else
            js_promise_resolve(rt, a->q.target, new_response(rt, a->status, a->headers, a->body, a->len, a->url));
        return;
    }
    jobj *x = a->q.target;
    jv xv = jv_obj(x);
    hidden(rt, x, "__id", jv_num(0));
    if (a->status <= 0) {
        js_set(rt, xv, rt->A.status, jv_num(0));
        xhr_state(rt, x, 4);
        xhr_fire(rt, x, "error");
        xhr_fire(rt, x, "loadend");
        return;
    }
    jv text = jv_str(js_str_utf8(rt, a->body ? a->body : "", a->body ? a->len : 0));
    hidden(rt, x, "__headers", new_headers(rt, a->headers));
    js_set(rt, xv, rt->A.status, jv_num(a->status));
    js_set(rt, xv, js_atom(rt, "statusText"), str8(rt, status_text(a->status)));
    js_set(rt, xv, js_atom(rt, "responseURL"), str8(rt, a->url ? a->url : ""));
    js_set(rt, xv, js_atom(rt, "responseText"), text);
    jv type = js_get(rt, xv, js_atom(rt, "responseType")), resp = text;
    if (type.t == JT_STR && js_str_is(type.u.s, "json")) {
        jv json = js_get(rt, jv_obj(rt->global), js_atom(rt, "JSON"));
        if (js_try_call(rt, js_get(rt, json, js_atom(rt, "parse")), json, 1, &text, &resp))
            resp = jv_null();                            /* zly JSON - response = null */
    }
    js_set(rt, xv, js_atom(rt, "response"), resp);
    xhr_state(rt, x, 2);
    xhr_state(rt, x, 3);
    xhr_state(rt, x, 4);
    xhr_fire(rt, x, "load");
    xhr_fire(rt, x, "loadend");
}

void js_request_done(struct js_rt *rt, int id, int status, const char *headers,
                     const char *body, unsigned len, const char *final_url)
{
    if (!rt || rt->dead)
        return;
    struct js_netreq *q = 0;
    for (int i = 0; i < JS_NET_MAX; i++)
        if (rt->net[i].state == 2 && rt->net[i].id == id)
            q = &rt->net[i];
    if (!q)
        return;                                          /* przerwane (abort) albo nieznane */
    struct done_args a = { *q, status, headers, body, final_url, len };
    q->state = 0;                                        /* miejsce wolne, zanim skrypt doda nowe */
    rt->script_name = a.q.xhr ? "XMLHttpRequest" : "fetch";
    rt->budget_ms = BUDGET_EVENT;
    rt->abort_msg = 0;
    rt->last_error[0] = '\0';
    js_protect(rt, done_cb, &a);
}

/* ---------- rejestracja ---------- */

static void ctor(js_rt *rt, const char *name, jnative call, jnative construct, jobj *proto)
{
    jobj *f = js_new_native(rt, name, call, 0);
    f->u.nf.ctor = construct;
    js_define(rt, f, rt->A.prototype, jv_obj(proto), 0);
    js_define(rt, proto, rt->A.constructor, jv_obj(f), PF_WRITE | PF_CONF);
    js_def_val(rt, rt->global, name, jv_obj(f));
}

static jv illegal(js_rt *rt, jv self U, int argc U, jv *argv U, jobj *fn U)
{
    js_throw_error(rt, JS_ERR_TYPE, "Illegal constructor");
}

void js_net_init(js_rt *rt)
{
    jobj *op = rt->object_proto;
    js_def_fn(rt, rt->global, "fetch", w_fetch, 1);

    jobj *hp = js_new_object(rt, op);
    rt->headers_proto = hp;
    js_def_fn(rt, hp, "get", h_get, 1);
    js_def_fn(rt, hp, "has", h_has, 1);
    js_def_fn(rt, hp, "forEach", h_for_each, 1);
    js_def_fn(rt, hp, "entries", h_entries, 0);
    ctor(rt, "Headers", illegal, 0, hp);

    jobj *rp = js_new_object(rt, op);
    rt->response_proto = rp;
    js_def_fn(rt, rp, "text", r_text, 0);
    js_def_fn(rt, rp, "json", r_json, 0);
    js_def_fn(rt, rp, "arrayBuffer", r_array_buffer, 0);
    js_def_fn(rt, rp, "clone", r_clone, 0);
    ctor(rt, "Response", illegal, 0, rp);

    jobj *xp = js_new_object(rt, op);
    rt->xhr_proto = xp;
    js_def_fn(rt, xp, "open", x_open, 2);
    js_def_fn(rt, xp, "send", x_send, 0);
    js_def_fn(rt, xp, "setRequestHeader", x_set_request_header, 2);
    js_def_fn(rt, xp, "abort", x_abort, 0);
    js_def_fn(rt, xp, "getResponseHeader", x_get_response_header, 1);
    js_def_fn(rt, xp, "getAllResponseHeaders", x_get_all_response_headers, 0);
    js_def_fn(rt, xp, "addEventListener", x_add_event_listener, 2);
    js_def_fn(rt, xp, "removeEventListener", x_remove_event_listener, 2);
    js_def_fn(rt, xp, "overrideMimeType", x_noop, 1);
    js_define(rt, xp, js_atom(rt, "UNSENT"), jv_num(0), 0);
    js_define(rt, xp, js_atom(rt, "OPENED"), jv_num(1), 0);
    js_define(rt, xp, js_atom(rt, "HEADERS_RECEIVED"), jv_num(2), 0);
    js_define(rt, xp, js_atom(rt, "LOADING"), jv_num(3), 0);
    js_define(rt, xp, js_atom(rt, "DONE"), jv_num(4), 0);
    ctor(rt, "XMLHttpRequest", xhr_call, xhr_ctor, xp);
}

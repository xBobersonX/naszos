/* Pobieranie stron WWW - patrz http.h.
 *
 * Rozmowa HTTP jest tekstowa. Wysylamy:
 *     GET /sciezka HTTP/1.0
 *     Host: example.com
 * a serwer odpowiada linia statusu ("HTTP/1.1 200 OK"), naglowkami
 * ("Content-Type: text/html") i po pustej linii - trescia strony.
 * HTTP/1.0 i "Connection: close": serwer wysyla calosc i zamyka polaczenie,
 * wiec koniec strony = koniec polaczenia. */

#include "http.h"
#include "net.h"
#include "tls.h"
#include "heap.h"
#include "string.h"

#define PAGE_MAX (2u * 1024 * 1024)      /* najwiecej pobieramy 2 MiB */

int str_starts_ci(const char *s, const char *prefix)
{
    for (; *prefix; s++, prefix++) {
        char a = *s, b = *prefix;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b)
            return 0;
    }
    return 1;
}

/* Bezpieczne sklejanie napisow */
static void append(char *out, unsigned size, const char *s)
{
    unsigned n = (unsigned)strlen(out);
    while (*s && n + 1 < size)
        out[n++] = *s++;
    out[n] = '\0';
}

/* ---------- adresy ---------- */

int url_parse(const char *s, struct url *u)
{
    while (*s == ' ')
        s++;
    u->https = 0;
    u->port = 80;
    if (str_starts_ci(s, "http://")) {
        s += 7;
    } else if (str_starts_ci(s, "https://")) {
        s += 8;
        u->https = 1;
        u->port = 443;
    }
    unsigned n = 0;
    while (*s && *s != '/' && *s != ':' && *s != '?' && *s != '#' && *s != ' ') {
        if (n + 1 >= sizeof(u->host))
            return 0;
        char c = *s++;
        u->host[n++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    u->host[n] = '\0';
    if (!n)
        return 0;
    if (*s == ':') {
        unsigned port = 0;
        for (s++; *s >= '0' && *s <= '9'; s++)
            port = port * 10 + (unsigned)(*s - '0');
        if (!port || port > 65535)
            return 0;
        u->port = (uint16_t)port;
    }
    n = 0;
    if (*s != '/')
        u->path[n++] = '/';
    /* Sciezka bez "#kotwicy"; spacje zamieniamy na %20 */
    while (*s && *s != '#' && n + 4 < sizeof(u->path)) {
        unsigned char ch = (unsigned char)*s;
        if (ch == ' ' || ch >= 0x80) {                  /* spacja, polskie litery -> %XX */
            static const char hex[] = "0123456789ABCDEF";
            u->path[n++] = '%';
            u->path[n++] = hex[ch >> 4];
            u->path[n++] = hex[ch & 15];
            s++;
        } else {
            u->path[n++] = *s++;
        }
    }
    u->path[n] = '\0';
    return 1;
}

void url_format(const struct url *u, char *out, unsigned size)
{
    if ((u->https && u->port == 443) || (!u->https && u->port == 80))
        ksnprintf(out, size, "%s://%s%s", u->https ? "https" : "http", u->host, u->path);
    else
        ksnprintf(out, size, "%s://%s:%u%s", u->https ? "https" : "http", u->host, u->port, u->path);
}

int url_resolve(const char *base, const char *href, char *out, unsigned size)
{
    while (*href == ' ')
        href++;
    if (str_starts_ci(href, "http://") || str_starts_ci(href, "https://")) {
        struct url u;
        if (!url_parse(href, &u))
            return 0;
        url_format(&u, out, size);
        return 1;
    }
    struct url b;
    if (!url_parse(base, &b))
        return 0;
    if (href[0] == '/' && href[1] == '/') {             /* "//host/sciezka" - ten sam protokol */
        char tmp[URL_MAX];
        ksnprintf(tmp, sizeof(tmp), "%s:%s", b.https ? "https" : "http", href);
        return url_resolve(base, tmp, out, size);
    }
    char path[sizeof(b.path)];
    if (href[0] == '/') {                               /* od katalogu glownego serwera */
        ksnprintf(path, sizeof(path), "%s", href);
    } else if (href[0] == '?') {                        /* ta sama strona, inne parametry */
        ksnprintf(path, sizeof(path), "%s", b.path);
        for (char *p = path; *p; p++)
            if (*p == '?') {
                *p = '\0';
                break;
            }
        append(path, sizeof(path), href);
    } else {                                            /* wzgledem biezacego katalogu */
        ksnprintf(path, sizeof(path), "%s", b.path);
        char *q = path;
        while (*q && *q != '?')
            q++;
        *q = '\0';
        char *slash = path;
        for (char *p = path; *p; p++)
            if (*p == '/')
                slash = p;
        slash[1] = '\0';
        /* "./" i "../" */
        while (href[0] == '.') {
            if (href[1] == '/') {
                href += 2;
            } else if (href[1] == '.' && (href[2] == '/' || href[2] == '\0')) {
                href += href[2] ? 3 : 2;
                unsigned l = (unsigned)strlen(path);
                if (l > 1) {
                    path[l - 1] = '\0';                  /* usun koncowy "/" */
                    char *s = path;
                    for (char *p = path; *p; p++)
                        if (*p == '/')
                            s = p;
                    s[1] = '\0';
                }
            } else {
                break;
            }
        }
        append(path, sizeof(path), href);
    }
    struct url r = b;
    ksnprintf(r.path, sizeof(r.path), "%s", path);
    for (char *p = r.path; *p; p++)                      /* bez kotwicy */
        if (*p == '#') {
            *p = '\0';
            break;
        }
    url_format(&r, out, size);
    return 1;
}

void url_encode(const char *s, char *out, unsigned size)
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned n = 0;
    for (; *s && n + 4 < size; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[n++] = (char)c;
        } else if (c == ' ') {
            out[n++] = '+';
        } else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 15];
        }
    }
    out[n] = '\0';
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void url_decode(const char *s, char *out, unsigned size)
{
    unsigned n = 0;
    while (*s && n + 1 < size) {
        if (*s == '%' && hex_value(s[1]) >= 0 && hex_value(s[2]) >= 0) {
            out[n++] = (char)(hex_value(s[1]) * 16 + hex_value(s[2]));
            s += 3;
        } else if (*s == '+') {
            out[n++] = ' ';
            s++;
        } else {
            out[n++] = *s++;
        }
    }
    out[n] = '\0';
}

/* ---------- pobieranie ---------- */

const char *http_strerror(int err)
{
    switch (err) {
    case HTTP_EHTTPS: return "strona szyfrowana (https) - nie udalo sie nawiazac TLS";
    case HTTP_EURL:   return "niepoprawny adres";
    case HTTP_ENOMEM: return "za malo pamieci";
    default:          return tls_strerror(err);
    }
}

/* Polaczenie zwykle (TCP) albo szyfrowane (TLS na TCP) - reszta kodu
 * nie musi wiedziec, ktore */
static int conn_send(int secure, const void *d, unsigned n)
{
    return secure ? tls_send(d, n) : tcp_send(d, n);
}

static int conn_recv(int secure, void *b, unsigned n, uint32_t timeout)
{
    return secure ? tls_recv(b, n, timeout) : tcp_recv(b, n, timeout);
}

static void conn_close(int secure)
{
    if (secure)
        tls_close();
    tcp_close();
}

static void progress(http_progress_fn fn, void *ctx, const char *msg)
{
    if (fn)
        fn(ctx, msg);
}

/* Wartosc naglowka (np. "location") z czesci naglowkowej odpowiedzi */
static int header(const char *head, const char *name, char *out, unsigned size)
{
    unsigned nl = (unsigned)strlen(name);
    for (const char *line = head; *line;) {
        if (str_starts_ci(line, name) && line[nl] == ':') {
            const char *v = line + nl + 1;
            while (*v == ' ')
                v++;
            unsigned n = 0;
            while (*v && *v != '\r' && *v != '\n' && n + 1 < size)
                out[n++] = *v++;
            out[n] = '\0';
            return 1;
        }
        while (*line && *line != '\n')
            line++;
        if (*line)
            line++;
    }
    return 0;
}

/* Jedno zapytanie (bez przekierowan). Wynik w r->raw / r->status */
static int fetch_once(const struct url *u, const struct http_req *q, struct http_response *r,
                      http_progress_fn fn, void *ctx)
{
    char msg[160], ipbuf[16];
    uint32_t ip;
    r->secure = 0;
    r->cipher[0] = '\0';
    ksnprintf(msg, sizeof(msg), "Szukam adresu %s...", u->host);
    progress(fn, ctx, msg);
    int err = net_resolve(u->host, &ip);
    if (err)
        return err;
    net_format_ip(ip, ipbuf);
    ksnprintf(msg, sizeof(msg), "Lacze sie z %s (%s)...", u->host, ipbuf);
    progress(fn, ctx, msg);
    err = tcp_connect(ip, u->port);
    if (err)
        return err;
    int secure = u->https;
    if (secure) {
        progress(fn, ctx, "Szyfruje polaczenie (TLS 1.3, X25519)...");
        err = tls_connect(u->host);
        if (err) {
            tcp_close();
            return err;
        }
        r->secure = 1;
        ksnprintf(r->cipher, sizeof(r->cipher), "%s", tls_cipher_name());
    }

    /* Naglowek zapytania. Zwykla strona: GET z naszymi naglowkami; fetch ze skryptu
     * moze podac metode, wlasne naglowki i tresc (POST) */
    const char *extra = q && q->headers ? q->headers : "";
    unsigned blen = q && q->body ? q->body_len : 0;
    unsigned cap_req = URL_MAX + 300 + (unsigned)strlen(extra);
    char *req = kmalloc(cap_req);
    if (!req) {
        conn_close(secure);
        return HTTP_ENOMEM;
    }
    int len = ksnprintf(req, cap_req, "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: NaszOS/3.0 (hobby OS)\r\n",
                        q && q->method ? q->method : "GET", u->path, u->host);
    if (!q)
        len += ksnprintf(req + len, cap_req - (unsigned)len, "Accept: text/html, text/plain\r\nAccept-Language: pl, en\r\n");
    else if (!str_starts_ci(extra, "accept:") && !strstr_ci(extra, "\naccept:"))
        len += ksnprintf(req + len, cap_req - (unsigned)len, "Accept: */*\r\n");
    if (q && (q->body || (q->method && strcmp(q->method, "GET") && strcmp(q->method, "HEAD"))))
        len += ksnprintf(req + len, cap_req - (unsigned)len, "Content-Length: %u\r\n", blen);
    len += ksnprintf(req + len, cap_req - (unsigned)len, "%sConnection: close\r\n\r\n", extra);
    err = conn_send(secure, req, (unsigned)len);
    kfree(req);
    if (!err && blen)
        err = conn_send(secure, q->body, blen);
    if (err) {
        conn_close(secure);
        return err;
    }
    progress(fn, ctx, "Czekam na odpowiedz serwera...");

    unsigned cap = 16384, size = 0, shown = 0;
    char *buf = kmalloc(cap);
    if (!buf) {
        conn_close(secure);
        return HTTP_ENOMEM;
    }
    for (;;) {
        if (size + 4096 > cap && cap < PAGE_MAX) {      /* bufor rosnie w miare potrzeby */
            char *bigger = kmalloc(cap * 2);
            if (!bigger)
                break;
            memcpy(bigger, buf, size);
            kfree(buf);
            buf = bigger;
            cap *= 2;
        }
        if (size + 1 >= cap)
            break;                                       /* za duza - reszte pomijamy */
        int n = conn_recv(secure, buf + size, cap - 1 - size, 10000);
        if (n <= 0) {
            if (n < 0 && size == 0) {
                kfree(buf);
                conn_close(secure);
                return n;
            }
            break;
        }
        size += (unsigned)n;
        if (size / 8192 != shown) {
            shown = size / 8192;
            ksnprintf(msg, sizeof(msg), "Pobieram %s... %u KB", u->host, size / 1024);
            progress(fn, ctx, msg);
        }
    }
    conn_close(secure);
    buf[size] = '\0';
    r->raw = buf;

    /* Status: "HTTP/1.1 200 OK" */
    r->status = 0;
    const char *sp = buf;
    while (*sp && *sp != ' ' && *sp != '\n')
        sp++;
    for (sp++; *sp >= '0' && *sp <= '9'; sp++)
        r->status = r->status * 10 + (unsigned)(*sp - '0');

    /* Naglowki od tresci oddziela pusta linia */
    r->body = buf + size;
    for (char *p = buf; *p; p++) {
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n') {
            p[0] = '\0';
            r->body = p + 4;
            break;
        }
        if (p[0] == '\n' && p[1] == '\n') {
            p[0] = '\0';
            r->body = p + 2;
            break;
        }
    }
    r->body_len = (unsigned)(buf + size - r->body);
    r->content_type[0] = '\0';
    header(buf, "content-type", r->content_type, sizeof(r->content_type));
    return 0;
}

int http_get(const char *address, struct http_response *r, http_progress_fn fn, void *ctx)
{
    return http_request(address, 0, r, fn, ctx);
}

int http_header(const struct http_response *r, const char *name, char *out, unsigned size)
{
    return r->raw ? header(r->raw, name, out, size) : 0;
}

int http_request(const char *address, const struct http_req *q, struct http_response *r,
                 http_progress_fn fn, void *ctx)
{
    memset(r, 0, sizeof(*r));
    struct url u;
    if (!url_parse(address, &u))
        return HTTP_EURL;
    url_format(&u, r->url, sizeof(r->url));

    net_lock();
    int err = 0;
    struct http_req get = { "GET", q ? q->headers : 0, 0, 0 };
    for (int redirects = 0;; redirects++) {
        err = fetch_once(&u, q, r, fn, ctx);
        if (err) {
            if (u.https)                 /* szyfrowanie nie wyszlo - adres dla planu B */
                url_format(&u, r->https_url, sizeof(r->https_url));
            break;
        }
        char location[URL_MAX], next[URL_MAX];
        if (r->status >= 300 && r->status < 400 && redirects < 5 &&
            header(r->raw, "location", location, sizeof(location)) &&
            url_resolve(r->url, location, next, sizeof(next)) && url_parse(next, &u)) {
            char msg[URL_MAX + 40];
            ksnprintf(msg, sizeof(msg), "Przekierowanie na %s", next);
            progress(fn, ctx, msg);
            kfree(r->raw);
            r->raw = 0;
            url_format(&u, r->url, sizeof(r->url));
            /* 301/302/303: dalej juz GET bez tresci (tak robia przegladarki); 307/308: to samo zapytanie */
            if (q && q->method && r->status != 307 && r->status != 308)
                q = &get;
            continue;
        }
        break;
    }
    net_unlock();
    if (err)
        http_free(r);
    return err;
}

void http_free(struct http_response *r)
{
    kfree(r->raw);
    r->raw = r->body = 0;
    r->body_len = 0;
}

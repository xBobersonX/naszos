#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>

/* Pobieranie stron WWW (HTTP/1.0 przez TCP z src/net.c). Uzywaja tego
 * komenda "www" w terminalu i Przegladarka na pulpicie. */

#define URL_MAX 600

struct url {
    char host[128];
    char path[URL_MAX - 140];
    uint16_t port;
    int https;
};

/* "http://host:port/sciezka", "host/sciezka" albo "host". 1 = OK */
int  url_parse(const char *s, struct url *u);
void url_format(const struct url *u, char *out, unsigned size);
/* Adres linku (href) wzgledem strony base -> pelny adres. 1 = OK */
int  url_resolve(const char *base, const char *href, char *out, unsigned size);
/* Koduje tekst do adresu: spacja -> +, znaki specjalne -> %XX */
void url_encode(const char *s, char *out, unsigned size);
/* Odwrotnie: %XX i + -> znaki */
void url_decode(const char *s, char *out, unsigned size);

struct http_response {
    unsigned status;                 /* np. 200 = OK, 404 = nie ma */
    char *raw;                       /* cala odpowiedz (kmalloc) */
    char *body;                      /* tresc - wskaznik do srodka raw */
    unsigned body_len;
    char content_type[100];
    char url[URL_MAX];               /* adres po przekierowaniach */
    char https_url[URL_MAX];         /* https:// ktorego nie udalo sie zaszyfrowac */
    int secure;                      /* strona przyszla szyfrowana (TLS) */
    char cipher[48];                 /* np. "TLS 1.3, X25519, ChaCha20-Poly1305" */
};

#define HTTP_EHTTPS  -20             /* nie udalo sie zaszyfrowac - adres w https_url */
#define HTTP_EURL    -21             /* niepoprawny adres */
#define HTTP_ENOMEM  -22

/* Komunikaty o postepie ("Lacze sie...") - moze byc NULL */
typedef void (*http_progress_fn)(void *ctx, const char *msg);

/* Pobiera strone (sam bierze net_lock). 0 = OK (tez dla 404 itp. - patrz status) */
int  http_get(const char *url, struct http_response *r, http_progress_fn progress, void *ctx);
void http_free(struct http_response *r);
const char *http_strerror(int err);

int  str_starts_ci(const char *s, const char *prefix);   /* bez rozrozniania wielkosci liter */

#endif

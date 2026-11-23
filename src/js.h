#ifndef JS_H
#define JS_H

#include <stdint.h>

/* JavaScript w NaszOS (src/js_*.c) - interfejs dla przegladarki i terminala.
 *
 * Kazda strona dostaje wlasny "swiat" JS (struct js_rt): obiekt window,
 * zmienne globalne, timery, pamiec. Swiat znika razem ze strona.
 * W tym interfejsie nie ma liczb zmiennoprzecinkowych - reszta kernela
 * jest kompilowana bez SSE i nie moze dostawac ani oddawac typu double. */

struct js_rt;
struct dom_doc;
struct dom_node;
struct css_ctx;

/* Co silnik moze poprosic od przegladarki (kazda funkcja moze byc NULL) */
struct js_host {
    void *ctx;
    void (*console)(void *ctx, int level, const char *text);   /* 0 log, 1 ostrzezenie, 2 blad */
    void (*alert)(void *ctx, const char *text);
    void (*navigate)(void *ctx, const char *url);               /* location.href = ... */
    void (*back)(void *ctx);                                    /* history.back() */
    void (*scroll_to)(void *ctx, int y);                        /* window.scrollTo */
    int  (*scroll_y)(void *ctx);
    /* Pobranie pliku (skrypt z src=). 0 = OK, *body z kmalloc */
    int  (*fetch)(void *ctx, const char *url, char **body, unsigned *len);
    /* Polozenie elementu na stronie (po ulozeniu). 0 = nie wiadomo */
    int  (*box)(void *ctx, struct dom_node *n, int *x, int *y, int *w, int *h);
    /* Piksele pobranego obrazka <img> (0xRRGGBB) - dla ctx.drawImage. 0 = jeszcze nie ma */
    int  (*image)(void *ctx, struct dom_node *img, int *w, int *h, const uint32_t **px);
    int view_w, view_h;                                         /* rozmiar obszaru strony */
};

/* doc i css moga byc NULL (np. komenda "js" w terminalu - bez strony) */
struct js_rt *js_new(struct dom_doc *doc, struct css_ctx *css, const char *url, const struct js_host *host);
/* Ile pamieci moze zajac JavaScript strony (domyslnie 256 MiB) */
void js_set_mem_limit(struct js_rt *rt, unsigned long long bytes);
void js_free(struct js_rt *rt);
void js_set_host(struct js_rt *rt, const struct js_host *host);
/* Przerwanie dlugiego skryptu z zewnatrz (np. zamkniete okno) */
void js_set_cancel(struct js_rt *rt, volatile int *flag);

/* Wykonanie skryptu (UTF-8). 0 = OK, -1 = blad (opis: js_last_error).
 * script: wezel <script>, ktory go zawiera (dla document.write) albo NULL */
int js_run(struct js_rt *rt, const char *code, unsigned len, const char *name, struct dom_node *script);
/* Jak js_run, ale wynik ostatniego wyrazenia jako tekst (terminal) */
int js_eval_text(struct js_rt *rt, const char *code, unsigned len, char *out, unsigned size);
const char *js_last_error(struct js_rt *rt);
/* Znacznik <script> strony (tresc albo src= pobrane przez host->fetch) */
int js_run_script(struct js_rt *rt, struct dom_node *script, const char *name);

/* Wszystkie skrypty strony juz wykonane: DOMContentLoaded i load */
void js_fire_loaded(struct js_rt *rt);
/* Klikniecie w element strony. 1 = strona zablokowala domyslna akcje */
int  js_click(struct js_rt *rt, struct dom_node *target, int x, int y);
/* Klawisz (kody z keyboard.h). 1 = strona zablokowala domyslna akcje */
int  js_key(struct js_rt *rt, int key);
int  js_wants_keys(struct js_rt *rt);          /* strona slucha klawiszy */
int  js_input_focused(struct js_rt *rt);       /* kursor w polu tekstowym strony */
/* Timery (setTimeout, setInterval, requestAnimationFrame) - wolac czesto */
void js_timers(struct js_rt *rt);
int  js_has_timers(struct js_rt *rt);
/* DOM zmieniony od ostatniego pytania (trzeba policzyc style i uklad) */
int  js_dirty(struct js_rt *rt);
/* Obrazek <img> sie pobral (ok = 1) albo nie (ok = 0): zdarzenie load / error */
void js_image_done(struct js_rt *rt, struct dom_node *img, int ok);

/* Zapytania sieciowe skryptu (fetch, XMLHttpRequest). Skrypt tylko je zapisuje;
 * przegladarka odbiera je po jednym, pobiera w tle (watek) i oddaje wynik. */
struct js_request {
    int id;
    char method[12];
    char url[600];                   /* pelny adres (wzgledem strony) */
    char *headers;                   /* "Nazwa: wartosc\r\n"... albo NULL (kmalloc - zwalnia odbiorca) */
    char *body;                      /* tresc (np. POST) albo NULL (kmalloc - zwalnia odbiorca) */
    unsigned body_len;
};
int  js_take_request(struct js_rt *rt, struct js_request *out);   /* 1 = jest nowe zapytanie */
/* Wynik zapytania. status 0 = blad sieci (fetch odrzuca Promise). headers: naglowki
 * odpowiedzi (pierwsza linia "HTTP/1.1 200 OK"), moga byc NULL */
void js_request_done(struct js_rt *rt, int id, int status, const char *headers,
                     const char *body, unsigned len, const char *final_url);
/* Link "javascript:kod" */
void js_run_url(struct js_rt *rt, const char *code);
/* Zmiana rozmiaru okna (window.innerWidth, zdarzenie resize) */
void js_resize(struct js_rt *rt, int view_w, int view_h);
/* Ile pamieci zajmuje swiat JS (w KB) */
unsigned js_memory_kb(struct js_rt *rt);

#endif

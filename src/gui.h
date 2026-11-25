#ifndef GUI_H
#define GUI_H

#include "gfx.h"

/* Pulpit z oknami. gui_run() pokazuje pulpit i wraca, gdy uzytkownik wybierze
 * "Tryb tekstowy" (albo gdy nie ma trybu graficznego - wtedy zwraca 0). */
int gui_run(void);

struct window;

/* Program dzialajacy w oknie. Wspolrzedne w click() sa wzgledem obszaru
 * roboczego okna (bez paska tytulu). Kazda funkcja moze byc NULL. */
struct app {
    void (*draw)(struct window *w, struct rect client);
    void (*click)(struct window *w, int x, int y, int double_click);
    void (*key)(struct window *w, int key);
    void (*destroy)(struct window *w);   /* zwolnienie w->state */
    void (*tick)(struct window *w);      /* wolane w kazdym obrocie petli pulpitu */
    void (*resize)(struct window *w);    /* okno zmienilo rozmiar (powiekszenie/przywrocenie) */
    void (*scroll)(struct window *w, int delta);   /* kolko myszy nad oknem (+ = w dol) */
    void (*drag)(struct window *w, int x, int y, int released);  /* ruch z wcisnietym lewym przyciskiem po click (np. zaznaczanie) */
    int live;                            /* odswiezaj co sekunde (np. zegar) */
    int wants_escape;                    /* Esc trafia do programu zamiast zamykac okno */
    int min_w, min_h;                    /* najmniejszy obszar roboczy przy zmianie rozmiaru */
};

struct window {
    int used;
    int minimized;
    int maximized;
    struct rect r;                       /* cale okno razem z paskiem tytulu */
    struct rect normal;                  /* polozenie sprzed powiekszenia */
    char title[48];
    const struct app *app;
    void *state;
};

#define GUI_TITLE_H 26

struct window *gui_open(const struct app *app, const char *title, int w, int h, void *state);
void gui_close(struct window *w);
void gui_redraw(struct window *w);
void gui_set_title(struct window *w, const char *title);
void gui_maximize(struct window *w);         /* na caly ekran (nad paskiem zadan) */
int  gui_is_focused(const struct window *w);
struct rect gui_client_size(const struct window *w);   /* obszar roboczy; x = y = 0 */
void gui_set_client_size(struct window *w, int cw, int ch);
struct rect gui_client_screen(const struct window *w);  /* obszar roboczy na ekranie */
void gui_mouse_client(const struct window *w, int *x, int *y);   /* kursor wzgledem obszaru roboczego */

/* Ustawienia pulpitu - zapisywane w /ustawienia.cfg */
struct gui_settings {
    int wallpaper;                       /* indeks w gui_wallpapers[] */
    int accent;                          /* indeks w gui_accents[] */
    int watermark;                       /* napis NaszOS na tapecie */
    int seconds;                         /* sekundy w zegarze na pasku zadan */
    int sound;                           /* dzwieki z glosniczka */
};

struct wallpaper { const char *name; uint32_t top, bottom; };
struct accent    { const char *name; uint32_t light, dark; };

#define GUI_WALLPAPERS 6
#define GUI_ACCENTS    6
extern const struct wallpaper gui_wallpapers[GUI_WALLPAPERS];
extern const struct accent gui_accents[GUI_ACCENTS];

struct gui_settings *gui_get_settings(void);
void gui_settings_load(void);
void gui_settings_save(void);
void gui_apply_settings(void);           /* po zmianie: odrysuj pulpit i zapisz na dysk */
uint32_t gui_accent_color(void);         /* kolor akcentu (jasniejszy odcien) */
uint32_t gui_accent_dark(void);

/* Programy w oknach */
void app_open_settings(void);
void app_open_about(void);
void app_open_calendar(void);
void app_open_files(void);
void app_open_snake(void);
void app_open_browser(void);          /* src/browser.c */
void app_open_piano(void);            /* src/piano.c */
/* Terminal: okno z wlasna konsola, w ktorym dziala fn(arg) jako osobne zadanie.
 * Okno zamyka sie samo, gdy fn sie skonczy. */
void app_open_terminal(const char *title, void (*fn)(void *), void *arg);
/* Program .exe we wlasnym oknie */
void app_open_program(const char *path);

#endif

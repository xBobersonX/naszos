/* Biblioteka dla programow NaszOS.
 *
 * Program to zwykly plik C z funkcja main(). build.ps1 kompiluje kazdy plik
 * programs\*.c do programs\nazwa.exe i przy starcie wgrywa go do /programy.
 * Program dziala w trybie uzytkownika (ring 3): nie ma dostepu do sprzetu ani
 * do pamieci systemu - wszystko robi przez ponizsze funkcje (int 0x80). */

#ifndef NASZOS_H
#define NASZOS_H

/* ---- tekst w oknie terminala ---- */
void print(const char *s);
void print_int(int n);
void print_char(char c);
void set_color(int fg, int bg);          /* kolory 0-15 jak w VGA */
void clear_screen(void);
int  read_line(char *buf, int size);     /* czyta linie z klawiatury (z echem), zwraca dlugosc */

enum { BLACK, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN, LIGHT_GREY, DARK_GREY, LIGHT_BLUE,
       LIGHT_GREEN, LIGHT_CYAN, LIGHT_RED, LIGHT_MAGENTA, YELLOW, WHITE };

/* ---- klawiatura ---- */
int  get_key(void);                      /* czeka na klawisz */
int  poll_key(void);                     /* klawisz albo -1, gdy nic nie wcisnieto */
#define KEY_UP    0x100
#define KEY_DOWN  0x101
#define KEY_LEFT  0x102
#define KEY_RIGHT 0x103

/* ---- czas i losowanie ---- */
void sleep_ms(int ms);
int  time_ms(void);                      /* milisekundy od uruchomienia systemu */
int  random(int max);                    /* 0 .. max-1 */

/* ---- grafika: plotno w oknie programu (tylko na pulpicie) ---- */
int  canvas_open(int width, int height); /* 0 = OK, -1 = brak pulpitu */
void canvas_fill(int x, int y, int w, int h, unsigned color);   /* kolor 0xRRGGBB */
void canvas_text(int x, int y, const char *s, unsigned color);
void canvas_show(void);                  /* pokaz narysowana klatke */

struct mouse { int x, y, buttons; };     /* buttons: 1 = lewy, 2 = prawy */
void get_mouse(struct mouse *m);

/* ---- koniec ---- */
void exit(int code) __attribute__((noreturn));

/* ---- przydatne ---- */
int  str_len(const char *s);
int  str_eq(const char *a, const char *b);

#endif

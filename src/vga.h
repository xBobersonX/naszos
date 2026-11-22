#ifndef VGA_H
#define VGA_H

#include <stdint.h>

/* Konsola = ekran tekstowy: siatka znakow z kolorami, kursor i biezacy
 * kolor. Jest jedna konsola "ekranowa" (monitor) i dowolnie wiele konsol
 * w pamieci - np. dla okien terminala na pulpicie. Wszystkie funkcje vga_*
 * i kprintf pisza do konsoli biezacego zadania (con_cur), wiec powloka czy
 * edytor dzialaja w oknie bez zadnych zmian. */
struct console {
    unsigned cols, rows;
    unsigned top;                  /* pierwszy wiersz tekstu (na ekranie 0 = pasek statusu) */
    unsigned row, col;
    uint8_t color;
    int cursor_hidden;
    uint16_t *cells;               /* konsola w pamieci: znak | kolor << 8; ekran: NULL */
    volatile int dirty;            /* tresc sie zmienila - okno trzeba odrysowac */

    /* Wejscie: konsola w oknie dostaje klawisze od pulpitu, nie z klawiatury */
    int has_input;
    volatile uint16_t input[64];
    volatile unsigned in_head, in_tail;
    volatile int closed;           /* okno zamkniete - programy w nim maja sie skonczyc */
    int close_toggle;              /* po zamknieciu: na przemian Ctrl+Q i "brak klawisza" */
    unsigned click_x, click_y;     /* ostatnie klikniecie mysza (w znakach) */

    void *cwd;                     /* katalog biezacy powloki w tej konsoli */

    /* Plotno graficzne programu (zamiast tekstu): piksele 0xRRGGBB */
    uint32_t *canvas;
    int canvas_w, canvas_h;
    volatile int canvas_resize;    /* pulpit ma dopasowac rozmiar okna */
    volatile int mouse_x, mouse_y, mouse_buttons;   /* mysz wzgledem plotna (od pulpitu) */
};

extern struct console *con_cur;
extern struct console con_screen;

/* Rozmiar biezacej konsoli w znakach. VGA_MAX_* to gorne granice - do tablic. */
#define VGA_WIDTH    (con_cur->cols)
#define VGA_HEIGHT   (con_cur->rows)
#define VGA_TOP      (con_cur->top)
#define VGA_MAX_COLS 480
#define VGA_MAX_ROWS 160

enum vga_color {
    VGA_BLACK = 0, VGA_BLUE, VGA_GREEN, VGA_CYAN, VGA_RED, VGA_MAGENTA,
    VGA_BROWN, VGA_LIGHT_GREY, VGA_DARK_GREY, VGA_LIGHT_BLUE, VGA_LIGHT_GREEN,
    VGA_LIGHT_CYAN, VGA_LIGHT_RED, VGA_LIGHT_MAGENTA, VGA_YELLOW, VGA_WHITE,
};

extern const uint32_t vga_palette[16];   /* kolory VGA jako 0xRRGGBB */

void vga_init(void);
/* Przelacza na tryb graficzny width x height i rysuje znaki sama.
 * 1 = udalo sie; 0 = karta tego nie obsluguje (zostaje tryb tekstowy 80x25). */
int  vga_set_graphics(unsigned width, unsigned height);
int  vga_is_graphics(void);
void vga_resolution(unsigned *width, unsigned *height);   /* w pikselach */

/* Konsole w pamieci (okna terminala) */
struct console *console_create(unsigned cols, unsigned rows);
void console_destroy(struct console *c);
int  console_resize(struct console *c, unsigned cols, unsigned rows);   /* 1 = udalo sie */
void console_push_key(struct console *c, int key);
int  console_pop_key(struct console *c);                   /* -1 = brak */

/* Pasek statusu na ekranie (wiersz 0) - zawsze ekran, niezaleznie od con_cur */
void vga_status_write(unsigned x, const char *s, enum vga_color fg, enum vga_color bg);
unsigned vga_screen_width(void);

/* Dla pulpitu graficznego: wstrzymanie rysowania ekranu (tekst dalej jest
 * zapamietywany), dostep do bufora ramki i czcionki 8x16 (256 znakow po 16 B) */
void vga_suspend(int on);
void vga_emergency(void);            /* blad krytyczny: pisz na ekran, pokaz konsole */
void vga_framebuffer(uint32_t **buf, unsigned *width, unsigned *height, unsigned *pitch);
const uint8_t *vga_font(void);

void vga_clear(void);   /* czysci obszar tekstu (bez paska statusu) */
void vga_set_color(enum vga_color fg, enum vga_color bg);
void vga_putc(char c);
void vga_write(const char *s);

/* Pozycja kursora (kolumna x, wiersz y) */
void vga_get_pos(unsigned *x, unsigned *y);
void vga_set_pos(unsigned x, unsigned y);
unsigned vga_width(void);
unsigned vga_height(void);
void vga_show_cursor(int visible);   /* np. gra chowa migajacy kursor tekstu */

/* Pisze w podanym miejscu ekranu, nie ruszajac kursora ani biezacego koloru. */
void vga_write_at(unsigned x, unsigned y, const char *s,
                  enum vga_color fg, enum vga_color bg);
void vga_put_at(unsigned x, unsigned y, char c, enum vga_color fg, enum vga_color bg);
void vga_fill_row(unsigned y, enum vga_color fg, enum vga_color bg);   /* spacje w calym wierszu */

/* Kursor myszy na ekranie tekstowym: pole (x, y) z odwroconymi kolorami.
 * refresh() rysuje go ponownie, jesli tresc pod nim zostala nadpisana. */
void vga_mouse_move(unsigned x, unsigned y);
void vga_mouse_refresh(void);

/* Zapamietanie i przywrocenie tresci konsoli (bez paska statusu) - np. dla
 * edytora, ktory zajmuje cala konsole, a po wyjsciu ma wrocic widok powloki.
 * Struktura jest duza (150 KB) - przydzielaj ja przez kmalloc, nie na stosie. */
struct vga_state {
    uint16_t cells[VGA_MAX_COLS * VGA_MAX_ROWS];
    unsigned row, col;
    uint8_t color;
};
void vga_save(struct vga_state *s);
void vga_restore(const struct vga_state *s);

/* Minimalny printf: %s %c %d %u %x %%, z szerokoscia pola np. %8u, %08x */
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif

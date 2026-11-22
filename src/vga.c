/* Konsole tekstowe - patrz vga.h.
 *
 * Konsola ekranowa (con_screen) ma dwa tryby:
 * - tekstowy (80x25): znaki wpisujemy do pamieci karty pod 0xB8000,
 *   a karta sama zamienia je na obraz;
 * - graficzny (np. 1920x1200): karta pokazuje tylko piksele, wiec kazdy znak
 *   rysujemy sami czcionka 9x16 skopiowana z karty przed zmiana trybu.
 * Konsole w pamieci (okna terminala) trzymaja znaki w tablicy cells[],
 * a rysuje je pulpit. Wszystkie zmiany znakow przechodza przez con_set(). */

#include <stdarg.h>
#include <stddef.h>
#include "vga.h"
#include "io.h"
#include "pci.h"
#include "heap.h"
#include "string.h"

#define CHAR_W 9
#define CHAR_H 16

struct console con_screen = { .cols = 80, .rows = 25, .top = 1, .color = 0x07 };
struct console *con_cur = &con_screen;

#define SCR_COLS (con_screen.cols)
#define SCR_ROWS (con_screen.rows)

static volatile uint16_t *const text_buf = (uint16_t *)0xB8000;

/* ---- tryb graficzny ekranu ---- */
static int gfx;
static uint16_t shadow[VGA_MAX_COLS * VGA_MAX_ROWS];   /* znaki na ekranie (w trybie graficznym) */
static uint32_t *fb;                                   /* bufor ramki: piksel = 0x00RRGGBB */
static unsigned fb_w, fb_h, fb_pitch;                  /* pitch w pikselach */
static unsigned x_off, y_off;                          /* margines, gdy ekran nie dzieli sie na znaki */
static uint8_t font[256 * CHAR_H];
static unsigned text_cursor = (unsigned)-1;            /* komorka z rysowanym kursorem tekstu */

/* Standardowa paleta 16 kolorow VGA */
const uint32_t vga_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF,
};

static inline uint16_t vga_entry(char c, uint8_t attr)
{
    return (uint16_t)(uint8_t)c | (uint16_t)attr << 8;
}

/* Szybkie kopiowanie i wypelnianie po 4 bajty (rep movsl/stosl) - bufor
 * ramki 1920x1200 to 9 MB, bajt po bajcie przewijanie trwaloby za dlugo */
static void copy32(uint32_t *dst, const uint32_t *src, unsigned long n)
{
    __asm__ volatile ("cld; rep movsl" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}

static void fill32(uint32_t *dst, uint32_t value, unsigned long n)
{
    __asm__ volatile ("cld; rep stosl" : "+D"(dst), "+c"(n) : "a"(value) : "memory");
}

/* Gdy pulpit graficzny zajmuje ekran, konsola ekranowa dalej zapamietuje tekst
 * (shadow), ale nie rysuje - po powrocie odrysowujemy wszystko naraz */
static int suspended;

static void render(unsigned i)
{
    if (suspended)
        return;
    unsigned cx = i % SCR_COLS, cy = i / SCR_COLS;
    uint8_t ch = (uint8_t)shadow[i], attr = (uint8_t)(shadow[i] >> 8);
    uint32_t fg = vga_palette[attr & 0x0F], bg = vga_palette[attr >> 4];
    uint32_t *p = fb + (y_off + cy * CHAR_H) * fb_pitch + x_off + cx * CHAR_W;
    const uint8_t *glyph = &font[ch * CHAR_H];
    /* Znaki ramek (0xC0-0xDF) przedluzaja 8. kolumne na 9., zeby linie sie laczyly - jak w karcie VGA */
    int line_char = ch >= 0xC0 && ch <= 0xDF;

    for (unsigned y = 0; y < CHAR_H; y++, p += fb_pitch) {
        uint8_t bits = glyph[y];
        if (i == text_cursor && y >= CHAR_H - 2)
            bits = 0xFF;                /* kursor tekstu: kreska na dole pola */
        for (unsigned x = 0; x < 8; x++)
            p[x] = (bits & (0x80 >> x)) ? fg : bg;
        p[8] = (line_char && (bits & 1)) || (i == text_cursor && y >= CHAR_H - 2) ? fg : bg;
    }
}

/* Pola ekranu (tylko con_screen) */
static uint16_t cell_get(unsigned i)
{
    return gfx ? shadow[i] : text_buf[i];
}

static void cell_set(unsigned i, uint16_t v)
{
    if (!gfx) {
        text_buf[i] = v;
    } else if (shadow[i] != v) {        /* rysujemy tylko to, co sie zmienilo */
        shadow[i] = v;
        render(i);
    }
}

/* Pola dowolnej konsoli */
static uint16_t con_get(struct console *c, unsigned i)
{
    return c->cells ? c->cells[i] : cell_get(i);
}

static void con_set(struct console *c, unsigned i, uint16_t v)
{
    if (c->cells) {
        c->cells[i] = v;
        c->dirty = 1;
    } else {
        cell_set(i, v);
    }
}

static void update_cursor(struct console *c)
{
    if (c->cells) {                     /* okno rysuje kursor samo */
        c->dirty = 1;
        return;
    }
    unsigned pos = c->cursor_hidden ? (unsigned)-1 : c->row * SCR_COLS + c->col;
    if (gfx) {
        if (pos == text_cursor)
            return;
        unsigned old = text_cursor;
        text_cursor = pos < SCR_COLS * SCR_ROWS ? pos : (unsigned)-1;
        if (old != (unsigned)-1)
            render(old);
        if (text_cursor != (unsigned)-1)
            render(text_cursor);
        return;
    }
    outb(0x3D4, 0x0A);                  /* bit 5 rejestru "Cursor Start" wylacza kursor */
    outb(0x3D5, c->cursor_hidden ? 0x20 : 0x0E);
    if (c->cursor_hidden)
        return;
    outb(0x3D4, 0x0F);
    outb(0x3D5, pos & 0xFF);
    outb(0x3D4, 0x0E);
    outb(0x3D5, (pos >> 8) & 0xFF);
}

void vga_show_cursor(int visible)
{
    con_cur->cursor_hidden = !visible;
    update_cursor(con_cur);
}

/* ---- Kursor myszy na ekranie tekstowym ----
 * Pole pod mysza rysujemy z zamienionymi kolorami tekstu i tla. Zapamietujemy
 * oryginal (mouse_saved) i to, co narysowalismy (mouse_drawn). Jesli ktos
 * w miedzyczasie nadpisze to pole (np. wypisze tam tekst), nie przywracamy
 * starego oryginalu - vga_mouse_refresh() narysuje kursor na nowej tresci. */

static int mouse_visible;
static unsigned mouse_x, mouse_y;
static uint16_t mouse_saved, mouse_drawn;

static void mouse_draw(void)
{
    if (!mouse_visible)
        return;
    unsigned i = mouse_y * SCR_COLS + mouse_x;
    uint16_t cell = cell_get(i);
    uint8_t attr = (uint8_t)(cell >> 8);
    uint8_t inv = (uint8_t)(attr << 4 | attr >> 4);
    if (inv == attr)                    /* np. czarne na czarnym - bylby niewidoczny */
        inv = 0x70;
    mouse_saved = cell;
    mouse_drawn = (uint16_t)((cell & 0xFF) | inv << 8);
    cell_set(i, mouse_drawn);
}

static void mouse_erase(void)
{
    if (!mouse_visible)
        return;
    unsigned i = mouse_y * SCR_COLS + mouse_x;
    if (cell_get(i) == mouse_drawn)
        cell_set(i, mouse_saved);
}

void vga_mouse_move(unsigned x, unsigned y)
{
    if (x >= SCR_COLS || y >= SCR_ROWS)
        return;
    unsigned long flags = irq_save();
    mouse_erase();
    mouse_x = x;
    mouse_y = y;
    mouse_visible = 1;
    mouse_draw();
    irq_restore(flags);
}

void vga_mouse_refresh(void)
{
    unsigned long flags = irq_save();
    if (mouse_visible && cell_get(mouse_y * SCR_COLS + mouse_x) != mouse_drawn)
        mouse_draw();                   /* tresc pod kursorem sie zmienila */
    irq_restore(flags);
}

/* ---- przewijanie i czyszczenie ---- */

static void scroll(struct console *c)
{
    unsigned first = c->top * c->cols, last = (c->rows - 1) * c->cols;
    uint16_t blank = vga_entry(' ', c->color);

    if (c->cells) {
        memmove(&c->cells[first], &c->cells[first + c->cols], (last - first) * sizeof(uint16_t));
        for (unsigned i = last; i < last + c->cols; i++)
            c->cells[i] = blank;
        c->row = c->rows - 1;
        c->dirty = 1;
        return;
    }

    unsigned long flags = irq_save();
    mouse_erase();                      /* inaczej odwrocone pole pojechaloby w gore z tekstem */
    if (gfx) {
        unsigned cur = text_cursor;     /* kursor tekstu nie moze pojechac w gore z obrazem */
        text_cursor = (unsigned)-1;
        if (cur != (unsigned)-1)
            render(cur);
        /* Przesuwamy gotowe piksele zamiast rysowac wszystkie znaki od nowa */
        if (!suspended) {
            uint32_t *dst = fb + (y_off + c->top * CHAR_H) * fb_pitch;
            uint32_t *src = dst + CHAR_H * fb_pitch;
            copy32(dst, src, (c->rows - 1 - c->top) * CHAR_H * fb_pitch);
        }
        for (unsigned i = first; i < last; i++)
            shadow[i] = shadow[i + c->cols];
        for (unsigned i = last; i < last + c->cols; i++) {
            shadow[i] = (uint16_t)~blank;   /* wymusza narysowanie */
            cell_set(i, blank);
        }
    } else {
        for (unsigned i = first; i < last; i++)
            text_buf[i] = text_buf[i + c->cols];
        for (unsigned i = last; i < last + c->cols; i++)
            text_buf[i] = blank;
    }
    c->row = c->rows - 1;
    mouse_draw();
    irq_restore(flags);
}

void vga_set_color(enum vga_color fg, enum vga_color bg)
{
    con_cur->color = (uint8_t)(fg | bg << 4);
}

void vga_clear(void)
{
    struct console *c = con_cur;
    unsigned long flags = irq_save();
    if (!c->cells)
        mouse_erase();                  /* kursor moze stac na pasku statusu, ktorego nie czyscimy */
    for (unsigned i = c->top * c->cols; i < c->cols * c->rows; i++)
        con_set(c, i, vga_entry(' ', c->color));
    if (!c->cells)
        mouse_draw();
    irq_restore(flags);
    c->row = c->top;
    c->col = 0;
    update_cursor(c);
}

/* ---- konsole w pamieci ---- */

struct console *console_create(unsigned cols, unsigned rows)
{
    struct console *c = kzalloc(sizeof(*c));
    if (!c)
        return 0;
    c->cells = kmalloc(cols * rows * sizeof(uint16_t));
    if (!c->cells) {
        kfree(c);
        return 0;
    }
    c->cols = cols;
    c->rows = rows;
    c->top = 0;
    c->color = 0x07;
    c->has_input = 1;
    for (unsigned i = 0; i < cols * rows; i++)
        c->cells[i] = vga_entry(' ', c->color);
    c->dirty = 1;
    return c;
}

/* Zmiana rozmiaru konsoli okna (np. po powiekszeniu). Tekst zostaje w lewym
 * gornym rogu; przy zmniejszaniu gubimy gorne wiersze, zeby wiersz z kursorem
 * dalej byl widoczny. Wolane z pulpitu, gdy program w konsoli czeka (oddal
 * procesor), wiec nikt w tym czasie nie pisze do cells[]. */
int console_resize(struct console *c, unsigned cols, unsigned rows)
{
    if (!c->cells || cols < 10 || rows < 3 || cols > VGA_MAX_COLS || rows > VGA_MAX_ROWS)
        return 0;
    if (cols == c->cols && rows == c->rows)
        return 1;
    uint16_t *cells = kmalloc(cols * rows * sizeof(uint16_t));
    if (!cells)
        return 0;
    uint16_t blank = vga_entry(' ', c->color);
    unsigned shift = c->row >= rows ? c->row - rows + 1 : 0;
    for (unsigned y = 0; y < rows; y++)
        for (unsigned x = 0; x < cols; x++) {
            unsigned sy = y + shift;
            cells[y * cols + x] = sy < c->rows && x < c->cols ? c->cells[sy * c->cols + x] : blank;
        }
    kfree(c->cells);
    c->cells = cells;
    c->cols = cols;
    c->rows = rows;
    c->row -= shift;
    if (c->col >= cols)
        c->col = cols - 1;
    c->dirty = 1;
    return 1;
}

void console_destroy(struct console *c)
{
    if (!c || c == &con_screen)
        return;
    kfree(c->cells);
    kfree(c);
}

void console_push_key(struct console *c, int key)
{
    unsigned next = (c->in_head + 1) % 64;
    if (next != c->in_tail) {
        c->input[c->in_head] = (uint16_t)key;
        c->in_head = next;
    }
}

int console_pop_key(struct console *c)
{
    if (c->in_head == c->in_tail)
        return -1;
    int key = c->input[c->in_tail];
    c->in_tail = (c->in_tail + 1) % 64;
    return key;
}

/* ---- pasek statusu (zawsze ekran) ---- */

void vga_status_write(unsigned x, const char *s, enum vga_color fg, enum vga_color bg)
{
    uint8_t attr = (uint8_t)(fg | bg << 4);
    for (; *s && x < SCR_COLS; s++, x++)
        cell_set(x, vga_entry(*s, attr));
}

unsigned vga_screen_width(void)
{
    return SCR_COLS;
}

/* ---- inicjalizacja ---- */

/* Domyslnie najwyzszy bit koloru tla oznacza "migaj", przez co jasne tla
 * (kolory 8-15) nie dzialaja, a tekst na nich miga. Przelaczamy go na
 * "jasne tlo" - bit 3 rejestru Mode Control w kontrolerze atrybutow VGA. */
static void disable_blink(void)
{
    inb(0x3DA);                         /* reset przerzutnika indeks/dane kontrolera atrybutow */
    outb(0x3C0, 0x10 | 0x20);           /* rejestr 0x10; 0x20 = obraz dalej wlaczony */
    uint8_t mode = inb(0x3C1);
    outb(0x3C0, mode & ~0x08);
}

/* Czcionke, ktora BIOS wgral do karty, trzyma "plaszczyzna 2" pamieci VGA
 * (32 bajty na znak, uzywane 16). Na chwile przestawiamy karte tak, zeby
 * byla widoczna pod 0xA0000, kopiujemy i przywracamy ustawienia trybu tekstowego. */
static void read_font(void)
{
    outw(0x3C4, 0x0402);                /* sekwenser: zapis tylko do plaszczyzny 2 */
    outw(0x3C4, 0x0704);                /* sekwenser: adresowanie liniowe */
    outw(0x3CE, 0x0204);                /* kontroler grafiki: odczyt z plaszczyzny 2 */
    outw(0x3CE, 0x0005);                /* bez przeplotu parzyste/nieparzyste */
    outw(0x3CE, 0x0406);                /* pamiec pod 0xA0000-0xAFFFF */

    const volatile uint8_t *src = (const volatile uint8_t *)0xA0000;
    for (unsigned c = 0; c < 256; c++)
        for (unsigned y = 0; y < CHAR_H; y++)
            font[c * CHAR_H + y] = src[c * 32 + y];

    outw(0x3C4, 0x0302);                /* przywrocenie ustawien trybu tekstowego */
    outw(0x3C4, 0x0304);
    outw(0x3CE, 0x0004);
    outw(0x3CE, 0x1005);
    outw(0x3CE, 0x0E06);
}

void vga_init(void)
{
    disable_blink();
    read_font();
    con_cur = &con_screen;
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    for (unsigned i = 0; i < con_screen.top * SCR_COLS; i++)
        cell_set(i, vga_entry(' ', con_screen.color));
    vga_clear();
}

/* ---- karta Bochs VBE (standardowa karta QEMU, Bochs, VirtualBox) ---- */

#define VBE_INDEX 0x01CE
#define VBE_DATA  0x01CF
#define VBE_ID          0
#define VBE_XRES        1
#define VBE_YRES        2
#define VBE_BPP         3
#define VBE_ENABLE      4
#define VBE_VIRT_WIDTH  6
#define VBE_VIDEO_MEM   0x0A            /* pamiec karty w blokach 64 KiB (nowsze wersje) */
#define VBE_ENABLED     0x01
#define VBE_LFB         0x40            /* liniowy bufor ramki */

static uint16_t vbe_read(uint16_t reg)
{
    outw(VBE_INDEX, reg);
    return inw(VBE_DATA);
}

static void vbe_write(uint16_t reg, uint16_t v)
{
    outw(VBE_INDEX, reg);
    outw(VBE_DATA, v);
}

/* Adres bufora ramki = BAR0 karty na szynie PCI (vendor 0x1234, device 0x1111) */
static uint32_t find_framebuffer(void)
{
    struct pci_device d;
    if (pci_find(0x1234, 0x1111, &d))
        return pci_read(&d, PCI_BAR0) & 0xFFFFFFF0;
    return 0;
}

int vga_set_graphics(unsigned width, unsigned height)
{
    uint16_t id = vbe_read(VBE_ID);
    if (id < 0xB0C0 || id > 0xB0CF)
        return 0;                       /* to nie jest karta Bochs VBE */
    if (width / CHAR_W > VGA_MAX_COLS || height / CHAR_H > VGA_MAX_ROWS ||
        width < 640 || height < 400)
        return 0;
    uint32_t vram = id >= 0xB0C5 ? (uint32_t)vbe_read(VBE_VIDEO_MEM) * 65536 : 16u << 20;
    if ((uint64_t)width * height * 4 > vram)
        return 0;                       /* za malo pamieci karty na taka rozdzielczosc */
    uint32_t addr = find_framebuffer();
    if (!addr)
        return 0;

    /* Zapamietujemy tresc ekranu tekstowego, zeby przeniesc ja do nowego trybu */
    static uint16_t old[80 * 25];
    for (unsigned i = 0; i < 80 * 25; i++)
        old[i] = text_buf[i];

    vbe_write(VBE_ENABLE, 0);
    vbe_write(VBE_XRES, (uint16_t)width);
    vbe_write(VBE_YRES, (uint16_t)height);
    vbe_write(VBE_BPP, 32);
    vbe_write(VBE_ENABLE, VBE_ENABLED | VBE_LFB);
    if (vbe_read(VBE_XRES) != width || vbe_read(VBE_YRES) != height) {
        vbe_write(VBE_ENABLE, 0);       /* karta nie przyjela trybu - wracamy do tekstu */
        return 0;
    }

    unsigned long flags = irq_save();
    fb = (uint32_t *)(uintptr_t)addr;
    fb_w = width;
    fb_h = height;
    fb_pitch = vbe_read(VBE_VIRT_WIDTH);
    con_screen.cols = width / CHAR_W;
    con_screen.rows = height / CHAR_H;
    x_off = (width - SCR_COLS * CHAR_W) / 2;
    y_off = (height - SCR_ROWS * CHAR_H) / 2;
    gfx = 1;
    text_cursor = (unsigned)-1;
    mouse_visible = 0;
    fill32(fb, 0, fb_pitch * fb_h);

    /* Wszystkie pola na czarno (spacja), potem stara tresc w lewym gornym rogu */
    uint16_t blank = vga_entry(' ', 0x07);
    for (unsigned i = 0; i < SCR_COLS * SCR_ROWS; i++) {
        shadow[i] = (uint16_t)~blank;
        cell_set(i, blank);
    }
    for (unsigned y = 0; y < 25; y++)
        for (unsigned x = 0; x < 80; x++)
            cell_set(y * SCR_COLS + x, old[y * 80 + x]);
    irq_restore(flags);
    update_cursor(&con_screen);
    return 1;
}

int vga_is_graphics(void)
{
    return gfx;
}

void vga_suspend(int on)
{
    unsigned long flags = irq_save();
    suspended = on;
    if (!on && gfx) {                   /* powrot z pulpitu - odrysowujemy cala konsole */
        fill32(fb, 0, fb_pitch * fb_h);
        for (unsigned i = 0; i < SCR_COLS * SCR_ROWS; i++)
            render(i);
    }
    irq_restore(flags);
}

void vga_emergency(void)
{
    con_cur = &con_screen;
    if (suspended)
        vga_suspend(0);
}

void vga_framebuffer(uint32_t **buf, unsigned *width, unsigned *height, unsigned *pitch)
{
    *buf = fb;
    *width = fb_w;
    *height = fb_h;
    *pitch = fb_pitch;
}

const uint8_t *vga_font(void)
{
    return font;
}

void vga_resolution(unsigned *width, unsigned *height)
{
    *width = gfx ? fb_w : 720;
    *height = gfx ? fb_h : 400;
}

/* ---- wypisywanie (do biezacej konsoli) ---- */

void vga_putc(char ch)
{
    struct console *c = con_cur;
    if (ch == '\n') {
        c->col = 0;
        c->row++;
    } else if (ch == '\b') {
        if (c->col > 0)
            con_set(c, c->row * c->cols + --c->col, vga_entry(' ', c->color));
    } else if (ch == '\t') {
        do
            vga_putc(' ');
        while (c->col % 4);
        return;
    } else {
        con_set(c, c->row * c->cols + c->col, vga_entry(ch, c->color));
        if (++c->col == c->cols) {
            c->col = 0;
            c->row++;
        }
    }
    if (c->row >= c->rows)
        scroll(c);
    update_cursor(c);
}

void vga_write(const char *s)
{
    while (*s)
        vga_putc(*s++);
}

void vga_get_pos(unsigned *x, unsigned *y)
{
    *x = con_cur->col;
    *y = con_cur->row;
}

void vga_set_pos(unsigned x, unsigned y)
{
    struct console *c = con_cur;
    if (x >= c->cols || y < c->top || y >= c->rows)
        return;
    c->col = x;
    c->row = y;
    update_cursor(c);
}

unsigned vga_width(void)  { return con_cur->cols; }
unsigned vga_height(void) { return con_cur->rows; }

void vga_put_at(unsigned x, unsigned y, char ch, enum vga_color fg, enum vga_color bg)
{
    struct console *c = con_cur;
    if (x < c->cols && y < c->rows)
        con_set(c, y * c->cols + x, vga_entry(ch, (uint8_t)(fg | bg << 4)));
}

void vga_fill_row(unsigned y, enum vga_color fg, enum vga_color bg)
{
    for (unsigned x = 0; x < con_cur->cols; x++)
        vga_put_at(x, y, ' ', fg, bg);
}

void vga_save(struct vga_state *s)
{
    struct console *c = con_cur;
    unsigned long flags = irq_save();
    if (!c->cells)
        mouse_erase();                  /* zapisujemy ekran bez kursora myszy */
    for (unsigned i = 0; i < c->cols * c->rows; i++)
        s->cells[i] = con_get(c, i);
    if (!c->cells)
        mouse_draw();
    irq_restore(flags);
    s->row = c->row;
    s->col = c->col;
    s->color = c->color;
}

void vga_restore(const struct vga_state *s)
{
    struct console *c = con_cur;
    unsigned long flags = irq_save();
    if (!c->cells)
        mouse_erase();
    for (unsigned i = c->top * c->cols; i < c->cols * c->rows; i++)
        con_set(c, i, s->cells[i]);
    if (!c->cells)
        mouse_draw();
    irq_restore(flags);
    c->row = s->row;
    c->col = s->col;
    c->color = s->color;
    update_cursor(c);
}

void vga_write_at(unsigned x, unsigned y, const char *s,
                  enum vga_color fg, enum vga_color bg)
{
    struct console *c = con_cur;
    uint8_t attr = (uint8_t)(fg | bg << 4);
    for (; *s && x < c->cols && y < c->rows; s++, x++)
        con_set(c, y * c->cols + x, vga_entry(*s, attr));
}

static void print_uint(uint64_t v, unsigned base, int width, char pad)
{
    char tmp[21];
    int i = 0;
    do {
        tmp[i++] = "0123456789abcdef"[v % base];
        v /= base;
    } while (v);
    for (int n = i; n < width; n++)
        vga_putc(pad);
    while (i--)
        vga_putc(tmp[i]);
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            vga_putc(*fmt);
            continue;
        }
        /* Opcjonalna szerokosc pola: %8u (spacje) albo %08x (zera) */
        char pad = ' ';
        int width = 0;
        if (*++fmt == '0')
            pad = '0';
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        /* %lu / %lx / %ld - liczby 64-bitowe (adresy, rejestry) */
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
        }

        switch (*fmt) {
        case 's': vga_write(va_arg(ap, const char *)); break;
        case 'c': vga_putc((char)va_arg(ap, int)); break;
        case 'u':
        case 'x': {
            uint64_t v = is_long ? va_arg(ap, uint64_t) : va_arg(ap, uint32_t);
            print_uint(v, *fmt == 'x' ? 16 : 10, width, pad);
            break;
        }
        case 'd': {
            int64_t v = is_long ? va_arg(ap, int64_t) : va_arg(ap, int32_t);
            if (v < 0) {
                vga_putc('-');
                print_uint(0u - (uint64_t)v, 10, width - 1, pad);
            } else {
                print_uint((uint64_t)v, 10, width, pad);
            }
            break;
        }
        case '%': vga_putc('%'); break;
        case '\0': fmt--; break;
        default: vga_putc('%'); vga_putc(*fmt); break;
        }
    }
    va_end(ap);
}

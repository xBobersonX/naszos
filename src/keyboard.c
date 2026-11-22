/* Sterownik klawiatury PS/2. Kontroler zglasza IRQ 1 przy kazdym nacisnieciu
 * i puszczeniu klawisza; z portu 0x60 czytamy scancode (zestaw 1, uklad US).
 * Przetlumaczone znaki (ASCII lub KEY_*) trafiaja do bufora cyklicznego. */

#include "crypto.h"
#include <stdint.h>
#include "keyboard.h"
#include "idt.h"
#include "io.h"
#include "vga.h"
#include "thread.h"

#define KBD_DATA   0x60
#define KBD_STATUS 0x64

#define SC_CTRL     0x1D    /* lewy; prawy to 0xE0 0x1D */
#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_CAPSLOCK 0x3A
#define SC_RELEASE  0x80    /* bit ustawiony przy puszczeniu klawisza */
#define SC_EXTENDED 0xE0    /* prefiks klawiszy rozszerzonych (strzalki itp.) */

static const char keymap[0x3A] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ',
};

static const char keymap_shift[0x3A] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ',
};

#define BUF_SIZE 256
static volatile uint16_t buf[BUF_SIZE];
static volatile uint32_t head, tail;

static int shift, ctrl, caps, extended;

static void buf_push(int key)
{
    uint32_t next = (head + 1) % BUF_SIZE;
    if (next != tail) {            /* pelny bufor - gubimy znak */
        buf[head] = (uint16_t)key;
        head = next;
    }
}

/* Klawisze nawigacyjne. Te same scancody wysyla klawiatura numeryczna
 * (bez prefiksu 0xE0) - traktujemy je tak samo, jak przy wylaczonym NumLocku. */
static int nav_key(uint8_t sc)
{
    switch (sc) {
    case 0x48: return KEY_UP;
    case 0x50: return KEY_DOWN;
    case 0x4B: return KEY_LEFT;
    case 0x4D: return KEY_RIGHT;
    case 0x47: return KEY_HOME;
    case 0x4F: return KEY_END;
    case 0x53: return KEY_DELETE;
    case 0x49: return KEY_PGUP;
    case 0x51: return KEY_PGDN;
    }
    return 0;
}

void keyboard_push(int key)
{
    buf_push(key);
}

static void on_key(struct regs *r)
{
    (void)r;
    if (inb(KBD_STATUS) & 0x20)        /* bajt od myszy, nie od klawiatury - odbierze go mouse.c */
        return;
    uint8_t sc = inb(KBD_DATA);
    random_mix(sc);                      /* chwila nacisniecia klawisza - troche losowosci */

    if (sc == SC_EXTENDED) {
        extended = 1;
        return;
    }
    if (extended) {
        extended = 0;
        if ((sc & ~SC_RELEASE) == SC_CTRL) {   /* prawy Ctrl */
            ctrl = !(sc & SC_RELEASE);
            return;
        }
        if (sc & SC_RELEASE)
            return;
        if (sc == 0x1C)            /* Enter z klawiatury numerycznej */
            buf_push('\n');
        else if (nav_key(sc))
            buf_push(nav_key(sc));
        return;
    }

    if (sc & SC_RELEASE) {
        sc &= ~SC_RELEASE;
        if (sc == SC_LSHIFT || sc == SC_RSHIFT)
            shift = 0;
        else if (sc == SC_CTRL)
            ctrl = 0;
        return;
    }

    switch (sc) {
    case SC_LSHIFT:
    case SC_RSHIFT:   shift = 1; return;
    case SC_CTRL:     ctrl = 1; return;
    case SC_CAPSLOCK: caps = !caps; return;
    }

    if (nav_key(sc)) {
        buf_push(nav_key(sc));
        return;
    }
    if (sc >= sizeof(keymap))
        return;
    char c = shift ? keymap_shift[sc] : keymap[sc];
    if (ctrl && keymap[sc] >= 'a' && keymap[sc] <= 'z') {
        buf_push(KEY_CTRL(keymap[sc]));
        return;
    }
    if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c ^= 0x20;                 /* Caps Lock odwraca wielkosc liter */
    if (c)
        buf_push(c);
}

void keyboard_init(void)
{
    while (inb(KBD_STATUS) & 1)    /* wyrzuc zalegle dane z kontrolera */
        inb(KBD_DATA);
    interrupt_register(IRQ_BASE + 1, on_key);
    irq_unmask(1);
}

int keyboard_poll(void)
{
    /* Program w oknie terminala dostaje klawisze od pulpitu przez swoja konsole.
     * Po zamknieciu okna dostaje Ctrl+Q - edytor, gry i powloka rozumieja to
     * jako "wyjdz". Na przemian z "brak klawisza": petle w stylu "czytaj,
     * dopoki cos jest" musza sie skonczyc, inaczej program nigdy nie
     * oddalby procesora i pulpit by zamarl. */
    if (con_cur->has_input) {
        if (con_cur->closed) {
            con_cur->close_toggle ^= 1;
            return con_cur->close_toggle ? KEY_CTRL('q') : -1;
        }
        return console_pop_key(con_cur);
    }

    if (head == tail)
        return -1;
    int key = buf[tail];
    tail = (tail + 1) % BUF_SIZE;
    return key;
}

int keyboard_getkey(void)
{
    int key;
    while ((key = keyboard_poll()) < 0)
        cpu_idle();                     /* inne zadania dzialaja, gdy czekamy */
    return key;
}

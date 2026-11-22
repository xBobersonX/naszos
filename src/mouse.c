/* Sterownik myszy PS/2. Mysz jest podlaczona do tego samego kontrolera co
 * klawiatura (drugi port), zglasza IRQ 12 i przy kazdym ruchu wysyla pakiet
 * 3 bajtow: przyciski + znaki, przesuniecie X, przesuniecie Y. Pozycje
 * trzymamy w pikselach ekranu (w trybie tekstowym 720x400, znak ma 9x16).
 * Konsola rysuje kursor na polu, w ktorym wypada mysz; pulpit graficzny
 * czyta pozycje w pikselach i zdarzenia przyciskow z kolejki. */

#include <stdint.h>
#include "mouse.h"
#include "idt.h"
#include "io.h"
#include "vga.h"
#include "keyboard.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL  0x02

#define CHAR_W 9
#define CHAR_H 16

static unsigned screen_w, screen_h;     /* rozmiar ekranu w pikselach (z vga_resolution) */
#define SCREEN_W screen_w
#define SCREEN_H screen_h

/* Kolejka zmian stanu przyciskow - pulpit nie przegapi krotkiego klikniecia */
#define EVENT_QUEUE 32
static struct mouse_event events[EVENT_QUEUE];
static volatile unsigned ev_head, ev_tail;

#define PKT_LEFT     0x01
#define PKT_ALWAYS1  0x08     /* bit zawsze ustawiony w 1. bajcie - do synchronizacji */
#define PKT_X_SIGN   0x10
#define PKT_Y_SIGN   0x20
#define PKT_OVERFLOW 0xC0

static uint8_t packet[4];
static int packet_idx;
static int packet_len = 3;              /* 4 = mysz z kolkiem (IntelliMouse) */
static volatile int wheel;              /* obroty kolka od ostatniego mouse_wheel() */
static int px, py;                      /* ustawiane w mouse_init() - rozmiar ekranu nie jest staly */
static uint8_t buttons;
static unsigned click_x, click_y;

static void wait_write(void)
{
    for (int i = 0; i < 100000 && (inb(PS2_STATUS) & STATUS_INPUT_FULL); i++)
        ;
}

static int wait_read(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(PS2_STATUS) & STATUS_OUTPUT_FULL)
            return 1;
    return 0;
}

/* Polecenie dla myszy (0xD4 = "nastepny bajt do drugiego portu"). 1 = mysz potwierdzila (0xFA) */
static int mouse_cmd(uint8_t cmd)
{
    wait_write();
    outb(PS2_COMMAND, 0xD4);
    wait_write();
    outb(PS2_DATA, cmd);
    return wait_read() && inb(PS2_DATA) == 0xFA;
}

static void on_mouse(struct regs *r)
{
    (void)r;
    if (!(inb(PS2_STATUS) & STATUS_OUTPUT_FULL))
        return;
    uint8_t b = inb(PS2_DATA);

    if (packet_idx == 0 && !(b & PKT_ALWAYS1))
        return;                         /* zgubilismy synchronizacje - czekamy na 1. bajt */
    packet[packet_idx++] = b;
    if (packet_idx < packet_len)
        return;
    packet_idx = 0;
    if (packet[0] & PKT_OVERFLOW)
        return;

    if (packet_len == 4)
        wheel += (int8_t)packet[3];      /* +1 = kolko do siebie (w dol strony) */

    int dx = packet[1] - ((packet[0] & PKT_X_SIGN) ? 256 : 0);
    int dy = packet[2] - ((packet[0] & PKT_Y_SIGN) ? 256 : 0);
    px += dx;
    py -= dy;                           /* dla myszy "w gore" to +Y, na ekranie - w dol */
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= (int)SCREEN_W) px = (int)SCREEN_W - 1;
    if (py >= (int)SCREEN_H) py = (int)SCREEN_H - 1;

    unsigned cx = (unsigned)px / CHAR_W, cy = (unsigned)py / CHAR_H;
    vga_mouse_move(cx, cy);

    if ((packet[0] & PKT_LEFT) && !(buttons & PKT_LEFT)) {   /* wlasnie wcisniety */
        click_x = cx;
        click_y = cy;
        keyboard_push(KEY_MOUSE_LEFT);
    }
    if ((packet[0] & MOUSE_BUTTONS) != (buttons & MOUSE_BUTTONS)) {
        unsigned next = (ev_head + 1) % EVENT_QUEUE;
        if (next != ev_tail) {
            events[ev_head].x = px;
            events[ev_head].y = py;
            events[ev_head].buttons = packet[0] & MOUSE_BUTTONS;
            ev_head = next;
        }
    }
    buttons = packet[0];
}

int mouse_init(void)
{
    vga_resolution(&screen_w, &screen_h);
    px = (int)SCREEN_W / 2;
    py = (int)SCREEN_H / 2;

    wait_write();
    outb(PS2_COMMAND, 0xA8);            /* wlacz drugi port (mysz) */

    wait_write();
    outb(PS2_COMMAND, 0x20);            /* odczyt bajtu konfiguracji kontrolera */
    if (!wait_read())
        return 0;
    uint8_t config = inb(PS2_DATA);
    config |= 0x02;                     /* przerwanie IRQ 12 od myszy */
    config &= ~0x20;                    /* nie wylaczaj zegara myszy */
    wait_write();
    outb(PS2_COMMAND, 0x60);
    wait_write();
    outb(PS2_DATA, config);

    if (!mouse_cmd(0xF6))                       /* ustawienia domyslne */
        return 0;
    /* "Magiczne haslo" myszy z kolkiem: czestotliwosc 200, 100, 80 - potem
     * mysz przedstawia sie numerem 3 i wysyla pakiety 4-bajtowe z kolkiem */
    if (mouse_cmd(0xF3) && mouse_cmd(200) && mouse_cmd(0xF3) && mouse_cmd(100) &&
        mouse_cmd(0xF3) && mouse_cmd(80) && mouse_cmd(0xF2) && wait_read() && inb(PS2_DATA) == 3)
        packet_len = 4;
    if (!mouse_cmd(0xF4))                       /* wlacz raportowanie */
        return 0;

    interrupt_register(IRQ_BASE + 12, on_mouse);
    irq_unmask(12);
    vga_mouse_move((unsigned)px / CHAR_W, (unsigned)py / CHAR_H);
    return 1;
}

void mouse_last_click(unsigned *x, unsigned *y)
{
    if (con_cur->has_input) {           /* program w oknie: pulpit przeliczyl klikniecie na znaki */
        *x = con_cur->click_x;
        *y = con_cur->click_y;
        return;
    }
    *x = click_x;
    *y = click_y;
}

void mouse_position(int *x, int *y)
{
    *x = px;
    *y = py;
}

int mouse_buttons(void)
{
    return buttons & MOUSE_BUTTONS;
}

int mouse_has_wheel(void)
{
    return packet_len == 4;
}

int mouse_wheel(void)
{
    unsigned long flags = irq_save();
    int w = wheel;
    wheel = 0;
    irq_restore(flags);
    return w;
}

int mouse_poll_event(struct mouse_event *e)
{
    if (ev_head == ev_tail)
        return 0;
    *e = events[ev_tail];
    ev_tail = (ev_tail + 1) % EVENT_QUEUE;
    return 1;
}

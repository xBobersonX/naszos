/* Snake w oknie pulpitu - wersja graficzna. Plansza to siatka pol po
 * CELL pikseli; waz rysowany jest jako ciagly pas z przejsciem kolorow od
 * glowy do ogona, glowa ma oczy patrzace w kierunku ruchu. Gra toczy sie
 * w tick() wolanym przez petle pulpitu i sama pauzuje, gdy okno traci fokus.
 * Rekord jest wspolny z wersja tekstowa (/snake_rekord.txt). */

#include <stdint.h>
#include "gui.h"
#include "gfx.h"
#include "keyboard.h"
#include "timer.h"
#include "heap.h"
#include "string.h"
#include "snake.h"
#include "speaker.h"

#define COLS    28
#define ROWS    20
#define CELL    22
#define HEADER  46
#define BOARD_W (COLS * CELL)
#define BOARD_H (ROWS * CELL)

#define START_SPEED 13          /* tykniec na krok (100 Hz -> 130 ms) */
#define MIN_SPEED   5

enum { READY, PLAYING, PAUSED, OVER };
enum { UP, RIGHT, DOWN, LEFT };

static const int DX[4] = { 0, 1, 0, -1 };
static const int DY[4] = { -1, 0, 1, 0 };

struct point { int8_t x, y; };

struct gsnake {
    struct point body[COLS * ROWS];     /* bufor cykliczny, head = glowa */
    int head, length;
    uint8_t occupied[ROWS][COLS];
    int dir;
    int turns[2], nturns;               /* skrety wcisniete miedzy krokami */
    struct point food;
    uint32_t score, record;
    int new_record;
    int state;
    uint32_t next_step;
    uint32_t rng;
};

static uint32_t rnd(struct gsnake *g)
{
    if (!g->rng)
        g->rng = 0x9E3779B9;
    g->rng ^= g->rng << 13;
    g->rng ^= g->rng >> 17;
    g->rng ^= g->rng << 5;
    return g->rng;
}

static struct point segment(const struct gsnake *g, int i)   /* 0 = glowa */
{
    return g->body[(g->head - i + COLS * ROWS) % (COLS * ROWS)];
}

static void place_food(struct gsnake *g)
{
    int free_cells = COLS * ROWS - g->length;
    if (free_cells <= 0)
        return;
    int pick = (int)(rnd(g) % (uint32_t)free_cells);
    for (int y = 0; y < ROWS; y++)
        for (int x = 0; x < COLS; x++)
            if (!g->occupied[y][x] && pick-- == 0) {
                g->food.x = (int8_t)x;
                g->food.y = (int8_t)y;
                return;
            }
}

static void new_game(struct gsnake *g)
{
    memset(g->occupied, 0, sizeof(g->occupied));
    g->length = 4;
    g->head = g->length - 1;
    for (int i = 0; i < g->length; i++) {
        g->body[i].x = (int8_t)(COLS / 2 - 3 + i);
        g->body[i].y = ROWS / 2;
        g->occupied[ROWS / 2][COLS / 2 - 3 + i] = 1;
    }
    g->dir = RIGHT;
    g->nturns = 0;
    g->score = 0;
    g->new_record = 0;
    g->rng ^= timer_ticks();
    place_food(g);
    g->state = PLAYING;
    g->next_step = timer_ticks() + START_SPEED;
}

static void game_over(struct gsnake *g)
{
    g->state = OVER;
    if (g->score > g->record) {
        g->record = g->score;
        g->new_record = 1;
        snake_save_record(g->record);
        sound_record();
    } else {
        sound_game_over();
    }
}

static void step(struct gsnake *g)
{
    if (g->nturns) {
        g->dir = g->turns[0];
        g->turns[0] = g->turns[1];
        g->nturns--;
    }
    struct point h = segment(g, 0);
    int nx = h.x + DX[g->dir], ny = h.y + DY[g->dir];
    if (nx < 0 || ny < 0 || nx >= COLS || ny >= ROWS) {
        game_over(g);
        return;
    }
    int eats = nx == g->food.x && ny == g->food.y;
    struct point tail = segment(g, g->length - 1);
    if (!eats)
        g->occupied[tail.y][tail.x] = 0;       /* ogon zwalnia pole w tym samym kroku */
    if (g->occupied[ny][nx]) {
        g->occupied[tail.y][tail.x] = 1;
        game_over(g);
        return;
    }
    g->head = (g->head + 1) % (COLS * ROWS);
    g->body[g->head].x = (int8_t)nx;
    g->body[g->head].y = (int8_t)ny;
    g->occupied[ny][nx] = 1;
    if (eats) {
        g->length++;
        g->score++;
        sound_eat();
        place_food(g);
    }
}

static uint32_t speed(const struct gsnake *g)
{
    uint32_t s = START_SPEED - g->score / 4;
    return (int)s < MIN_SPEED || s > START_SPEED ? MIN_SPEED : s;
}

/* ---------- rysowanie ---------- */

static void draw_apple(int x, int y)
{
    static const uint8_t widths[] = { 6, 10, 12, 14, 14, 14, 14, 12, 12, 10, 6 };
    int top = y + 7;
    for (unsigned r = 0; r < sizeof(widths); r++)
        gfx_fill(x + (CELL - widths[r]) / 2, top + (int)r, widths[r], 1, 0xDC2626);
    gfx_fill(x + 7, top + 2, 3, 3, 0xFCA5A5);                /* odblask */
    gfx_fill(x + 10, y + 3, 2, 5, 0x78350F);                 /* ogonek */
    gfx_fill(x + 12, y + 3, 5, 3, 0x22C55E);                 /* listek */
}

static void draw_snake(struct gsnake *g, int bx, int by)
{
    for (int i = g->length - 1; i >= 0; i--) {
        struct point p = segment(g, i);
        uint32_t color = gfx_mix(0x86EFAC, 0x15803D, i, g->length);
        int x = bx + p.x * CELL, y = by + p.y * CELL;
        gfx_fill(x + 3, y + 3, CELL - 6, CELL - 6, color);
        if (i > 0) {                         /* laczymy z segmentem blizej glowy */
            struct point n = segment(g, i - 1);
            int dx = n.x - p.x, dy = n.y - p.y;
            if (dx)
                gfx_fill(x + (dx > 0 ? CELL - 3 : -3), y + 3, 6, CELL - 6, color);
            if (dy)
                gfx_fill(x + 3, y + (dy > 0 ? CELL - 3 : -3), CELL - 6, 6, color);
        }
    }

    struct point h = segment(g, 0);           /* glowa: troche wieksza, z oczami */
    int x = bx + h.x * CELL, y = by + h.y * CELL;
    gfx_fill(x + 2, y + 2, CELL - 4, CELL - 4, 0xA3E635);
    int ex[2], ey[2];
    int cx = x + CELL / 2, cy = y + CELL / 2;
    if (g->dir == UP || g->dir == DOWN) {
        ex[0] = cx - 6; ex[1] = cx + 2;
        ey[0] = ey[1] = cy + (g->dir == UP ? -5 : 1);
    } else {
        ey[0] = cy - 6; ey[1] = cy + 2;
        ex[0] = ex[1] = cx + (g->dir == LEFT ? -5 : 1);
    }
    for (int k = 0; k < 2; k++) {
        gfx_fill(ex[k], ey[k], 5, 5, 0xFFFFFF);
        gfx_fill(ex[k] + 1 + DX[g->dir], ey[k] + 1 + DY[g->dir], 3, 3, 0x111827);
    }
}

static void overlay(int bx, int by, const char *title, const char *l1, const char *l2, const char *l3)
{
    gfx_darken(bx, by, BOARD_W, BOARD_H);
    int mid = by + BOARD_H / 2 - 50;
    int tw = gfx_text_width(title) * 3;
    gfx_text_scaled(bx + (BOARD_W - tw) / 2 + 2, mid + 2, title, 0x000000, 3);
    gfx_text_scaled(bx + (BOARD_W - tw) / 2, mid, title, 0xBEF264, 3);
    const char *lines[3] = { l1, l2, l3 };
    for (int i = 0; i < 3; i++)
        if (lines[i])
            gfx_text(bx + (BOARD_W - gfx_text_width(lines[i])) / 2, mid + 64 + i * 22, lines[i], 0xF3F4F6);
}

static void gsnake_draw(struct window *w, struct rect c)
{
    struct gsnake *g = w->state;
    char buf[40];

    gfx_gradient(c.x, c.y, c.w, HEADER, 0x166534, 0x052E16);
    gfx_text_scaled(c.x + 14, c.y + 7, "SNAKE", 0xBEF264, 2);
    ksnprintf(buf, sizeof(buf), "Wynik: %u", g->score);
    gfx_text(c.x + 190, c.y + 15, buf, 0xFFFFFF);
    ksnprintf(buf, sizeof(buf), "Rekord: %u", g->record);
    gfx_text(c.x + 320, c.y + 15, buf, 0xFDE68A);
    ksnprintf(buf, sizeof(buf), "Poziom: %u", (START_SPEED - speed(g)) + 1);
    gfx_text(c.x + c.w - 14 - gfx_text_width(buf), c.y + 15, buf, 0xBBF7D0);

    /* Plansza ma staly rozmiar - w powiekszonym oknie stoi na srodku */
    gfx_fill(c.x, c.y + HEADER, c.w, c.h - HEADER, 0x0B2A20);
    int bx = c.x + (c.w > BOARD_W ? (c.w - BOARD_W) / 2 : 0);
    int by = c.y + HEADER + (c.h - HEADER > BOARD_H ? (c.h - HEADER - BOARD_H) / 2 : 0);
    for (int y = 0; y < ROWS; y++)                           /* szachownica */
        for (int x = 0; x < COLS; x++)
            gfx_fill(bx + x * CELL, by + y * CELL, CELL, CELL, (x + y) % 2 ? 0x1B4D3E : 0x1F5A48);

    if (g->state != READY) {
        draw_apple(bx + g->food.x * CELL, by + g->food.y * CELL);
        draw_snake(g, bx, by);
    }

    switch (g->state) {
    case READY:
        overlay(bx, by, "SNAKE", "Strzalki - sterowanie   P - pauza",
                "Zjadaj jablka, nie uderzaj w sciany ani w siebie", "Enter albo klikniecie - start");
        break;
    case PAUSED:
        overlay(bx, by, "PAUZA", "P, Enter albo klikniecie - graj dalej", 0, 0);
        break;
    case OVER:
        ksnprintf(buf, sizeof(buf), "Wynik: %u", g->score);
        overlay(bx, by, "KONIEC GRY", buf, g->new_record ? "Nowy rekord!" : 0, "Enter albo klikniecie - jeszcze raz");
        break;
    }
}

/* ---------- sterowanie ---------- */

static void start_or_resume(struct window *w)
{
    struct gsnake *g = w->state;
    if (g->state == READY || g->state == OVER)
        new_game(g);
    else if (g->state == PAUSED) {
        g->state = PLAYING;
        g->next_step = timer_ticks() + speed(g);
    }
    gui_redraw(w);
}

static void gsnake_key(struct window *w, int key)
{
    struct gsnake *g = w->state;
    int d = -1;
    switch (key) {
    case KEY_UP:    d = UP; break;
    case KEY_RIGHT: d = RIGHT; break;
    case KEY_DOWN:  d = DOWN; break;
    case KEY_LEFT:  d = LEFT; break;
    case 'p': case 'P':
        if (g->state == PLAYING) {
            g->state = PAUSED;
            gui_redraw(w);
        } else if (g->state == PAUSED) {
            start_or_resume(w);
        }
        return;
    case '\n': case ' ':
        if (g->state != PLAYING)
            start_or_resume(w);
        return;
    default:
        return;
    }
    if (g->state != PLAYING || g->nturns == 2)
        return;
    int last = g->nturns ? g->turns[g->nturns - 1] : g->dir;
    if (d != last && d != (last + 2) % 4)            /* nie zawracamy w miejscu */
        g->turns[g->nturns++] = d;
}

static void gsnake_click(struct window *w, int x, int y, int dbl)
{
    (void)x; (void)dbl;
    struct gsnake *g = w->state;
    if (y >= HEADER && g->state != PLAYING)
        start_or_resume(w);
}

static void gsnake_tick(struct window *w)
{
    struct gsnake *g = w->state;
    if (g->state != PLAYING)
        return;
    if (!gui_is_focused(w)) {                        /* inne okno na wierzchu - pauza */
        g->state = PAUSED;
        gui_redraw(w);
        return;
    }
    if ((int32_t)(timer_ticks() - g->next_step) < 0)
        return;
    step(g);
    g->next_step = timer_ticks() + speed(g);
    gui_redraw(w);
}

static void gsnake_destroy(struct window *w)
{
    struct gsnake *g = w->state;
    if (g->score > g->record)                        /* zamkniete w trakcie gry z rekordem */
        snake_save_record(g->score);
    kfree(g);
}

static const struct app gsnake_app = {
    .draw = gsnake_draw, .click = gsnake_click, .key = gsnake_key,
    .destroy = gsnake_destroy, .tick = gsnake_tick, .min_w = BOARD_W, .min_h = HEADER + BOARD_H,
};

void app_open_snake(void)
{
    struct gsnake *g = kzalloc(sizeof(*g));
    if (!g)
        return;
    g->record = snake_load_record();
    g->rng = timer_ticks() * 2654435761u + 1;
    g->state = READY;
    if (!gui_open(&gsnake_app, "Snake", BOARD_W, HEADER + BOARD_H, g))
        kfree(g);
}

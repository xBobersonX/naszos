/* Gra Snake. Plansza to obszar tekstowy ekranu otoczony ramka. Tempo gry
 * wyznacza zegar PIT - waz robi krok co `step_ticks` tykniec, a miedzy
 * krokami oddajemy procesor (cpu_idle) i budzimy sie na przerwania.
 * Rekord jest zapisywany w pliku /snake_rekord.txt, wiec trafia na dysk. */

#include <stdint.h>
#include "snake.h"
#include "vga.h"
#include "keyboard.h"
#include "timer.h"
#include "fs.h"
#include "storage.h"
#include "string.h"
#include "thread.h"
#include "heap.h"
#include "speaker.h"

/* Plansza ma klasyczny rozmiar (najwyzej 80x23 razem z ramka) i stoi na
 * srodku ekranu - na duzym ekranie graficznym gra bylaby inaczej za wolna.
 * TOP/BOTTOM/LEFT/RIGHT to pozycje ramki, ustawiane w snake_run(). */
#define BOARD_MAX_W 80
#define BOARD_MAX_H 23
#define BAR_ROW     (VGA_HEIGHT - 1)
#define MAX_LEN     ((BOARD_MAX_W - 2) * (BOARD_MAX_H - 2))

static unsigned TOP, BOTTOM, LEFT, RIGHT;

#define START_SPEED 12            /* tykniec na krok (100 Hz -> 120 ms) */
#define MIN_SPEED   4

#define WALL_CHAR  ((char)0xDB)   /* pelny blok z zestawu znakow VGA (CP437) */
#define HEAD_CHAR  '@'
#define BODY_CHAR  'o'
#define FOOD_CHAR  '*'

#define RECORD_FILE "/snake_rekord.txt"

struct point { uint16_t x, y; };

enum dir { UP, DOWN, LEFT_D, RIGHT_D };

static struct point body[MAX_LEN];       /* bufor cykliczny: head = najnowszy segment */
static uint32_t head, length;
static uint8_t occupied[VGA_MAX_ROWS][VGA_MAX_COLS];
static struct point food;
static enum dir dir, next_dir;
static uint32_t score, record;
static uint32_t rng_state;

/* ---------- losowanie (xorshift32) ---------- */

static uint32_t rng(void)
{
    if (rng_state == 0)                  /* xorshift z zera nigdy by nie wyszedl */
        rng_state = 0x12345678;
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* ---------- rekord na dysku ---------- */

uint32_t snake_load_record(void)
{
    struct fs_node *f;
    uint32_t v = 0;
    if (fs_lookup(fs_root(), RECORD_FILE, &f) != FS_OK || f->is_dir)
        return 0;
    for (uint32_t i = 0; i < f->size && f->data[i] >= '0' && f->data[i] <= '9'; i++)
        v = v * 10 + (uint32_t)(f->data[i] - '0');
    return v;
}

void snake_save_record(uint32_t v)
{
    char buf[12];
    int n = 0;
    char tmp[11];
    int t = 0;
    do {
        tmp[t++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (t--)
        buf[n++] = tmp[t];
    buf[n++] = '\n';

    struct fs_node *f;
    int err = fs_lookup(fs_root(), RECORD_FILE, &f);
    if (err == FS_ENOENT)
        err = fs_create(fs_root(), RECORD_FILE, 0, &f);
    if (err == FS_OK && fs_write(f, buf, (uint32_t)n, 0) == FS_OK)
        storage_sync_if_dirty();
}

/* ---------- rysowanie ---------- */

static void put(uint16_t x, uint16_t y, char c, enum vga_color fg)
{
    vga_put_at(x, y, c, fg, VGA_BLACK);
}

static void draw_bar(const char *msg)
{
    char line[VGA_MAX_COLS + 1];
    memset(line, ' ', VGA_WIDTH);
    line[VGA_WIDTH] = '\0';

    /* " Wynik: 12   Rekord: 30        <komunikat>" */
    const char *parts[] = { " Wynik: ", 0, "   Rekord: ", 0, "     ", msg };
    char num[2][11];
    uint32_t vals[2] = { score, record };
    for (int i = 0; i < 2; i++) {
        char tmp[11];
        int t = 0, n = 0;
        uint32_t v = vals[i];
        do {
            tmp[t++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        while (t--)
            num[i][n++] = tmp[t];
        num[i][n] = '\0';
    }
    parts[1] = num[0];
    parts[3] = num[1];

    size_t pos = 0;
    for (int i = 0; i < 6; i++)
        for (const char *s = parts[i]; *s && pos < VGA_WIDTH; s++)
            line[pos++] = *s;

    for (unsigned x = 0; x < VGA_WIDTH; x++)
        vga_put_at(x, BAR_ROW, line[x], VGA_BLACK, VGA_LIGHT_GREEN);
}

static void draw_board(void)
{
    for (unsigned y = TOP; y <= BOTTOM; y++)
        for (unsigned x = LEFT; x <= RIGHT; x++) {
            int wall = y == TOP || y == BOTTOM || x == LEFT || x == RIGHT;
            put((uint16_t)x, (uint16_t)y, wall ? WALL_CHAR : ' ', VGA_DARK_GREY);
        }
}

/* Komunikat na srodku planszy */
static void center_text(unsigned y, const char *s, enum vga_color fg)
{
    unsigned len = (unsigned)strlen(s);
    unsigned x = (VGA_WIDTH - len) / 2;
    for (unsigned i = 0; i < len; i++)
        vga_put_at(x + i, y, s[i], fg, VGA_BLACK);
}

/* ---------- logika gry ---------- */

static void place_food(void)
{
    uint32_t free_cells = (RIGHT - LEFT - 1) * (BOTTOM - TOP - 1) - length;
    if (free_cells == 0)
        return;
    uint32_t pick = rng() % free_cells;         /* wybieramy n-te wolne pole */
    for (uint16_t y = TOP + 1; y < BOTTOM; y++)
        for (uint16_t x = LEFT + 1; x < RIGHT; x++)
            if (!occupied[y][x] && pick-- == 0) {
                food.x = x;
                food.y = y;
                put(x, y, FOOD_CHAR, VGA_LIGHT_RED);
                return;
            }
}

static void new_game(void)
{
    memset(occupied, 0, sizeof(occupied));
    draw_board();

    length = 3;
    head = 2;
    uint16_t cy = (TOP + BOTTOM) / 2, cx = (LEFT + RIGHT) / 2;
    for (uint32_t i = 0; i < length; i++) {
        body[i].x = (uint16_t)(cx - 2 + i);
        body[i].y = cy;
        occupied[cy][body[i].x] = 1;
        put(body[i].x, cy, i == head ? HEAD_CHAR : BODY_CHAR, VGA_LIGHT_GREEN);
    }
    dir = next_dir = RIGHT_D;
    score = 0;
    place_food();
    draw_bar("Strzalki - ruch   P - pauza   Q - wyjscie");
}

/* Jeden krok weza. Zwraca 0, gdy waz sie rozbil. */
static int step(void)
{
    dir = next_dir;
    struct point h = body[head];
    switch (dir) {
    case UP:      h.y--; break;
    case DOWN:    h.y++; break;
    case LEFT_D:  h.x--; break;
    case RIGHT_D: h.x++; break;
    }

    int eats = h.x == food.x && h.y == food.y;
    uint32_t tail = (head + MAX_LEN - (length - 1)) % MAX_LEN;

    /* Ogon zwalnia pole w tym samym kroku, wiec mozna w nie wejsc - chyba ze waz wlasnie rosnie */
    if (!eats) {
        occupied[body[tail].y][body[tail].x] = 0;
        put(body[tail].x, body[tail].y, ' ', VGA_BLACK);
    }
    if (h.x <= LEFT || h.x >= RIGHT || h.y <= TOP || h.y >= BOTTOM || occupied[h.y][h.x])
        return 0;

    put(body[head].x, body[head].y, BODY_CHAR, VGA_LIGHT_GREEN);
    head = (head + 1) % MAX_LEN;
    body[head] = h;
    occupied[h.y][h.x] = 1;
    put(h.x, h.y, HEAD_CHAR, VGA_LIGHT_GREEN);

    if (eats) {
        length++;
        score++;
        sound_eat();
        draw_bar("Strzalki - ruch   P - pauza   Q - wyjscie");
        place_food();
    }
    return 1;
}

static int opposite(enum dir a, enum dir b)
{
    return (a == UP && b == DOWN) || (a == DOWN && b == UP) ||
           (a == LEFT_D && b == RIGHT_D) || (a == RIGHT_D && b == LEFT_D);
}

static int is_quit(int key)
{
    return key == 'q' || key == 'Q' || key == 27 || key == KEY_CTRL('q');
}

/* Czeka na klawisz: 1 = nowa gra, 0 = wyjscie */
static int game_over(void)
{
    int new_record = score > record;
    if (new_record) {
        record = score;
        snake_save_record(record);
    }
    if (new_record)
        sound_record();
    else
        sound_game_over();
    center_text((TOP + BOTTOM) / 2 - 1, "  K O N I E C   G R Y  ", VGA_YELLOW);
    center_text((TOP + BOTTOM) / 2 + 1, new_record ? "  Nowy rekord!  " : "", VGA_LIGHT_CYAN);
    draw_bar("Enter - jeszcze raz   Q - wyjscie");

    for (;;) {
        int key = keyboard_getkey();
        if (key == '\n' || key == ' ')
            return 1;
        if (is_quit(key))
            return 0;
    }
}

void snake_run(void)
{
    static int active;                   /* stan gry jest wspolny - jedna gra naraz */
    if (active) {
        kprintf("Snake juz dziala w innym oknie.\n");
        return;
    }
    struct vga_state *saved = kmalloc(sizeof(*saved));
    if (!saved) {
        kprintf("snake: brak pamieci\n");
        return;
    }
    active = 1;

    vga_save(saved);
    vga_show_cursor(0);                          /* kursor tekstu nie jest potrzebny w grze */
    record = snake_load_record();
    rng_state = timer_ticks() * 2654435761u + 1;

    /* Plansza na srodku miejsca miedzy paskiem statusu a paskiem wyniku */
    unsigned avail_h = VGA_HEIGHT - VGA_TOP - 1;
    unsigned w = VGA_WIDTH < BOARD_MAX_W ? VGA_WIDTH : BOARD_MAX_W;
    unsigned h = avail_h < BOARD_MAX_H ? avail_h : BOARD_MAX_H;
    LEFT = (VGA_WIDTH - w) / 2;
    RIGHT = LEFT + w - 1;
    TOP = VGA_TOP + (avail_h - h) / 2;
    BOTTOM = TOP + h - 1;
    for (unsigned y = VGA_TOP; y < BAR_ROW; y++)
        vga_fill_row(y, VGA_LIGHT_GREY, VGA_BLACK);

    while (keyboard_poll() >= 0)                 /* wyrzucamy klawisze wcisniete przed gra */
        ;

    new_game();
    int running = 1;
    int paused = 0;
    uint32_t next_tick = timer_ticks();

    while (running) {
        int key;
        while ((key = keyboard_poll()) >= 0) {
            rng_state ^= timer_ticks();              /* dodatkowa losowosc z chwil wcisniecia */
            enum dir d = dir;
            int turn = 1;
            switch (key) {
            case KEY_UP:    d = UP; break;
            case KEY_DOWN:  d = DOWN; break;
            case KEY_LEFT:  d = LEFT_D; break;
            case KEY_RIGHT: d = RIGHT_D; break;
            default:        turn = 0; break;
            }
            if (turn && !paused && !opposite(d, dir))
                next_dir = d;
            else if (key == 'p' || key == 'P') {
                paused = !paused;
                draw_bar(paused ? "PAUZA - wcisnij P, aby grac dalej"
                                : "Strzalki - ruch   P - pauza   Q - wyjscie");
            } else if (is_quit(key)) {
                running = 0;
            }
        }
        if (!running)
            break;

        if (!paused && (int32_t)(timer_ticks() - next_tick) >= 0) {
            uint32_t speed = score / 3 < START_SPEED - MIN_SPEED ? START_SPEED - score / 3 : MIN_SPEED;
            next_tick = timer_ticks() + speed;
            if (!step()) {
                if (!game_over())
                    break;
                new_game();
                next_tick = timer_ticks();
            }
        }
        cpu_idle();
    }

    vga_restore(saved);
    kfree(saved);
    vga_show_cursor(1);
    active = 0;
    if (score > record) {                         /* wyjscie w trakcie gry z nowym rekordem */
        record = score;
        snake_save_record(record);
    }
}

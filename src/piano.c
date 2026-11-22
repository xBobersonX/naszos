/* Pianino - okno z klawiatura fortepianu (2 oktawy).
 *
 * Klawisze komputera jak w programach do muzyki ("trackerach"):
 *   dolny rzad  Z S X D C V G B H N J M  = oktawa od C4
 *   gorny rzad  Q 2 W 3 E R 5 T 6 Y 7 U  = oktawa od C5
 * (litery na bialych klawiszach, cyfry/S,D,G,H,J na czarnych).
 * Czestotliwosci liczymy bez liczb z przecinkiem: w tablicy jest oktawa
 * od C5 w setnych czesciach herca, a nizsza oktawa to polowa. */

#include "gui.h"
#include "gfx.h"
#include "heap.h"
#include "string.h"
#include "timer.h"
#include "audio.h"
#include "speaker.h"

#define KEYS       24                   /* dwie oktawy */
#define WHITE_W    40
#define WHITE_H    170
#define BLACK_W    26
#define BLACK_H    105
#define TOP        56
#define MARGIN     16
#define PIANO_W    (14 * WHITE_W + 2 * MARGIN)
#define PIANO_H    (TOP + WHITE_H + 44)
#define LIT_TICKS  20                   /* klawisz podswietlony 0,2 s */

/* C5..B5 w setnych Hz */
static const uint32_t octave5[12] = {
    52325, 55437, 58733, 62225, 65926, 69846, 73999, 78399, 83061, 88000, 93233, 98777,
};
static const char keymap[KEYS + 1] = "zsxdcvgbhnjmq2w3er5t6y7u";
static const int is_black[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
static const char *const note_names[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "H",
};

static const struct { const char *name; int wave; uint16_t ms; } instruments[] = {
    { "Pianino", WAVE_PIANO,    900 },
    { "Flet",    WAVE_TRIANGLE, 400 },
    { "8-bit",   WAVE_SQUARE,   250 },
    { "Organy",  WAVE_SINE,     500 },
};
#define INSTRUMENTS 4

struct piano {
    int instrument;
    uint32_t lit[KEYS];                 /* do kiedy klawisz jest podswietlony */
    int last;                           /* ostatnia nuta (do podpisu) */
};

static uint16_t key_freq(int k)
{
    uint32_t f = octave5[k % 12];
    if (k < 12)
        f /= 2;                          /* oktawa nizej = polowa czestotliwosci */
    return (uint16_t)((f + 50) / 100);
}

/* Polozenie klawisza na ekranie (wzgledem obszaru okna) */
static struct rect key_rect(int k)
{
    int octave = k / 12, n = k % 12;
    static const int white_index[12] = { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 };
    int wi = octave * 7 + white_index[n];
    struct rect r;
    if (is_black[n]) {
        r.x = MARGIN + (wi + 1) * WHITE_W - BLACK_W / 2;
        r.y = TOP;
        r.w = BLACK_W;
        r.h = BLACK_H;
    } else {
        r.x = MARGIN + wi * WHITE_W;
        r.y = TOP;
        r.w = WHITE_W;
        r.h = WHITE_H;
    }
    return r;
}

static struct rect instrument_rect(int i)
{
    struct rect r = { MARGIN + i * 110, 14, 100, 28 };
    return r;
}

static void play(struct window *w, int k)
{
    struct piano *p = w->state;
    if (audio_available() && sound_enabled())
        audio_note(key_freq(k), instruments[p->instrument].ms, instruments[p->instrument].wave, 80);
    else
        sound_beep(key_freq(k), 200);            /* bez karty - glosniczek PC */
    p->lit[k] = timer_ticks() + LIT_TICKS;
    p->last = k;
    gui_redraw(w);
}

static void piano_draw(struct window *w, struct rect c)
{
    struct piano *p = w->state;
    uint32_t now = timer_ticks();
    gfx_fill(c.x, c.y, c.w, c.h, 0x1F2937);
    for (int i = 0; i < INSTRUMENTS; i++) {
        struct rect r = instrument_rect(i);
        int sel = i == p->instrument;
        gfx_fill(c.x + r.x, c.y + r.y, r.w, r.h, sel ? gui_accent_color() : 0x374151);
        gfx_frame(c.x + r.x, c.y + r.y, r.w, r.h, sel ? 0xFFFFFF : 0x6B7280);
        gfx_text(c.x + r.x + (r.w - gfx_text_width(instruments[i].name)) / 2, c.y + r.y + 6,
                 instruments[i].name, 0xFFFFFF);
    }
    /* Najpierw biale, potem czarne (leza na wierzchu) */
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < KEYS; k++) {
            if (is_black[k % 12] != pass)
                continue;
            struct rect r = key_rect(k);
            int lit = (int32_t)(p->lit[k] - now) > 0;
            uint32_t color = pass ? (lit ? gui_accent_dark() : 0x111111) : (lit ? gui_accent_color() : 0xFFFFFF);
            gfx_fill(c.x + r.x, c.y + r.y, r.w, r.h, color);
            gfx_frame(c.x + r.x, c.y + r.y, r.w, r.h, 0x4B5563);
            char label[2] = { (char)(keymap[k] >= 'a' ? keymap[k] - 32 : keymap[k]), 0 };
            gfx_text(c.x + r.x + (r.w - 8) / 2, c.y + r.y + r.h - 22, label,
                     pass ? 0xD1D5DB : 0x6B7280);
        }
    char info[64];
    if (p->last >= 0)
        ksnprintf(info, sizeof(info), "Nuta: %s%u (%u Hz)", note_names[p->last % 12],
                  p->last / 12 + 4, key_freq(p->last));
    else
        ksnprintf(info, sizeof(info), "Graj klawiszami Z-M i Q-U albo myszka");
    gfx_text(c.x + MARGIN, c.y + TOP + WHITE_H + 14, info, 0xE5E7EB);
    if (!audio_available())
        gfx_text(c.x + c.w - 250, c.y + TOP + WHITE_H + 14, "(brak karty - glosniczek PC)", 0xFCA5A5);
}

static void piano_click(struct window *w, int x, int y, int dbl)
{
    (void)dbl;
    struct piano *p = w->state;
    for (int i = 0; i < INSTRUMENTS; i++) {
        struct rect r = instrument_rect(i);
        if (rect_contains(&r, x, y)) {
            p->instrument = i;
            gui_redraw(w);
            return;
        }
    }
    for (int pass = 1; pass >= 0; pass--)          /* czarne leza na wierzchu */
        for (int k = 0; k < KEYS; k++) {
            struct rect r = key_rect(k);
            if (is_black[k % 12] == pass && rect_contains(&r, x, y)) {
                play(w, k);
                return;
            }
        }
}

static void piano_key(struct window *w, int key)
{
    struct piano *p = w->state;
    if (key >= 'A' && key <= 'Z')
        key += 32;
    for (int k = 0; k < KEYS; k++)
        if (keymap[k] == key) {
            play(w, k);
            return;
        }
    if (key == '\t') {                                      /* Tab - nastepny instrument */
        p->instrument = (p->instrument + 1) % INSTRUMENTS;
        gui_redraw(w);
    }
}

static void piano_tick(struct window *w)
{
    struct piano *p = w->state;
    uint32_t now = timer_ticks();
    for (int k = 0; k < KEYS; k++)
        if (p->lit[k] && (int32_t)(p->lit[k] - now) <= 0) {   /* zgasl klawisz */
            p->lit[k] = 0;
            gui_redraw(w);
        }
}

static void piano_destroy(struct window *w)
{
    kfree(w->state);
}

static const struct app piano_app = {
    .draw = piano_draw, .click = piano_click, .key = piano_key, .tick = piano_tick,
    .destroy = piano_destroy, .min_w = PIANO_W, .min_h = PIANO_H,
};

void app_open_piano(void)
{
    struct piano *p = kzalloc(sizeof(*p));
    if (!p)
        return;
    p->last = -1;
    if (!gui_open(&piano_app, "Pianino", PIANO_W, PIANO_H, p))
        kfree(p);
}

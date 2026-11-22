/* Nuty i melodie. Gdy jest karta dzwiekowa (AC'97), nuty graja przez mikser
 * (audio.c) - kilka sciezek naraz, ladniejszym brzmieniem. Bez karty gramy
 * na PC Speakerze: jest podlaczony do kanalu 2 zegara PIT, ktory generuje
 * fale prostokatna o zadanej czestotliwosci, a bity 0 i 1 portu 0x61
 * wlaczaja kanal 2 i lacza go z glosniczkiem. Glosniczek umie tylko jeden
 * dzwiek naraz, wiec gra tylko sciezke 0. */

#include "speaker.h"
#include "io.h"
#include "timer.h"
#include "thread.h"
#include "audio.h"

#define PIT_BASE_HZ  1193182
#define PIT_CHANNEL2 0x42
#define PIT_CMD      0x43
#define SPEAKER_PORT 0x61

#define QUEUE 128

/* Kazda sciezka ma wlasna kolejke nut */
static struct track {
    struct note queue[QUEUE];
    volatile unsigned head, tail;
    volatile uint32_t remaining;          /* tykniec do konca biezacej nuty */
    int wave, volume;
} tracks[SOUND_TRACKS];
static volatile int playing;
static int enabled = 1;

static void tone_on(uint16_t freq)
{
    uint32_t divisor = PIT_BASE_HZ / freq;
    outb(PIT_CMD, 0xB6);                  /* kanal 2, lo/hi bajt, fala prostokatna */
    outb(PIT_CHANNEL2, divisor & 0xFF);
    outb(PIT_CHANNEL2, (divisor >> 8) & 0xFF);
    outb(SPEAKER_PORT, inb(SPEAKER_PORT) | 0x03);
}

static void tone_off(void)
{
    outb(SPEAKER_PORT, inb(SPEAKER_PORT) & ~0x03);
}

static void track_tick(int i)
{
    struct track *t = &tracks[i];
    if (t->remaining && --t->remaining)
        return;
    int card = audio_available();
    if (t->head == t->tail) {             /* kolejka pusta - cisza */
        if (i == 0 && playing && !card) {
            tone_off();
            playing = 0;
        }
        return;
    }
    struct note n = t->queue[t->tail];
    t->tail = (t->tail + 1) % QUEUE;
    if (card) {
        if (n.freq >= 20)                 /* troche krocej - nuty nie zlewaja sie w jedna */
            audio_note(n.freq, n.ms > 30 ? n.ms - 10 : n.ms, t->wave, t->volume);
    } else if (i == 0) {
        if (n.freq >= 20)
            tone_on(n.freq);
        else
            tone_off();
        playing = 1;
    }
    t->remaining = (n.ms * TIMER_HZ + 999) / 1000;
    if (t->remaining == 0)
        t->remaining = 1;
}

/* Z przerwania zegara (TIMER_HZ razy na sekunde) */
void sound_tick(void)
{
    for (int i = 0; i < SOUND_TRACKS; i++)
        track_tick(i);
}

void sound_play_track(int track, const struct note *notes, int count, int wave, int volume)
{
    if (!enabled || track < 0 || track >= SOUND_TRACKS)
        return;
    unsigned long flags = irq_save();
    struct track *t = &tracks[track];
    t->wave = wave;
    t->volume = volume;
    for (int i = 0; i < count; i++) {
        unsigned next = (t->head + 1) % QUEUE;
        if (next == t->tail)
            break;                        /* kolejka pelna - reszta przepada */
        t->queue[t->head] = notes[i];
        t->head = next;
    }
    irq_restore(flags);
}

void sound_play(const struct note *notes, int count)
{
    sound_play_track(0, notes, count, WAVE_SQUARE, 45);
}

void sound_beep(uint16_t freq, uint16_t ms)
{
    struct note n = { freq, ms };
    sound_play(&n, 1);
}

void sound_stop(void)
{
    unsigned long flags = irq_save();
    for (int i = 0; i < SOUND_TRACKS; i++) {
        tracks[i].head = tracks[i].tail = 0;
        tracks[i].remaining = 0;
    }
    playing = 0;
    tone_off();
    audio_stop_all();
    irq_restore(flags);
}

static int busy(void)
{
    for (int i = 0; i < SOUND_TRACKS; i++)
        if (tracks[i].head != tracks[i].tail || tracks[i].remaining)
            return 1;
    return 0;
}

void sound_wait(void)
{
    while (busy())
        cpu_idle();
}

void sound_set_enabled(int on)
{
    enabled = on;
    if (!on)
        sound_stop();
}

int sound_enabled(void)
{
    return enabled;
}

/* ---- gotowe efekty ---- */

/* Czestotliwosci nut (Hz), oktawy 4-6 */
enum {
    G2 = 98, B2 = 123, C3 = 131,
    C4 = 262, D4 = 294, E4 = 330, F4 = 349, G4 = 392, A4 = 440, B4 = 494,
    C5 = 523, D5 = 587, E5 = 659, F5 = 698, G5 = 784, A5 = 880, B5 = 988,
    C6 = 1047, E6 = 1319, G6 = 1568,
};

#define PLAY(...) do { static const struct note n[] = { __VA_ARGS__ }; \
                       sound_play(n, (int)(sizeof(n) / sizeof(n[0]))); } while (0)

void sound_startup(void)
{
    PLAY({ C5, 90 }, { E5, 90 }, { G5, 90 }, { C6, 220 });
}

void sound_shutdown(void)
{
    PLAY({ C6, 90 }, { G5, 90 }, { E5, 90 }, { C5, 260 });
}

void sound_eat(void)
{
    PLAY({ A5, 30 }, { E6, 40 });
}

void sound_game_over(void)
{
    PLAY({ G4, 160 }, { 0, 30 }, { F4, 160 }, { 0, 30 }, { E4, 160 }, { 0, 30 }, { C4, 420 });
}

void sound_record(void)
{
    PLAY({ C5, 100 }, { E5, 100 }, { G5, 100 }, { 0, 40 }, { E5, 100 }, { G5, 100 }, { C6, 120 },
         { 0, 40 }, { G6, 350 });
}

/* "Wlazl kotek na plotek" - melodia (pianino) i bas (trojkat) */
void sound_melody(void)
{
    static const struct note melody[] = {
        { G4, 300 }, { E4, 300 }, { E4, 300 }, { F4, 300 }, { D4, 300 }, { D4, 300 },
        { C4, 150 }, { E4, 150 }, { G4, 600 }, { 0, 150 },
        { G4, 300 }, { E4, 300 }, { E4, 300 }, { F4, 300 }, { D4, 300 }, { D4, 300 },
        { C4, 150 }, { E4, 150 }, { C4, 600 },
    };
    static const struct note bass[] = {
        { C3, 600 }, { G2, 300 }, { B2, 300 }, { G2, 600 }, { C3, 300 }, { G2, 600 }, { 0, 150 },
        { C3, 600 }, { G2, 300 }, { B2, 300 }, { G2, 600 }, { G2, 300 }, { C3, 600 },
    };
    sound_play_track(0, melody, (int)(sizeof(melody) / sizeof(melody[0])), WAVE_PIANO, 70);
    sound_play_track(1, bass, (int)(sizeof(bass) / sizeof(bass[0])), WAVE_TRIANGLE, 55);
}

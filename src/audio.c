/* Mikser dzwieku - patrz audio.h.
 *
 * Dzwiek cyfrowy to ciag liczb (probek): 48000 na sekunde, kazda mowi, jak
 * mocno wychylic membrane glosnika. Ton o czestotliwosci 440 Hz to fala,
 * ktora powtarza sie 440 razy na sekunde. Dla kazdego glosu trzymamy "faze"
 * - licznik, ktory przy kazdej probce rosnie o krok zalezny od
 * czestotliwosci; z fazy wyliczamy wartosc fali. Glosy dodajemy do siebie
 * i to jest caly mikser.
 *
 * Bez liczb z przecinkiem: faza to 32-bitowa liczba (pelny obrot = 2^32),
 * a glosnosci to ulamki "na 32768". */

#include "audio.h"
#include "ac97.h"
#include "io.h"
#include "string.h"

#define VOICES   12
#define ATTACK   (AC97_RATE / 200)        /* 5 ms narastania - bez trzaskow */
#define RELEASE  (AC97_RATE / 100)        /* 10 ms wyciszania na koncu */

struct voice {
    int active, wave;
    uint32_t phase, step;
    uint32_t age, length;                 /* probki: ile juz gra / ile ma grac */
    int32_t volume;                       /* 0..32767 */
};

static struct voice voices[VOICES];
static int available;
static uint32_t noise = 0x12345678;

/* Nagranie (jedno naraz) */
static struct {
    const int16_t *data;
    uint32_t frames;
    int channels;
    uint64_t pos, step;                   /* pozycja w nagraniu, ulamek 32.32 */
    int32_t volume;
    volatile int active;
} pcm;

int audio_available(void) { return available; }

/* Parabola zamiast prawdziwego sinusa - brzmi niemal tak samo, a liczy sie
 * na liczbach calkowitych. Wynik -32768..32767. */
static int32_t sine(uint32_t phase)
{
    int32_t x = (int32_t)((phase >> 16) & 0x7FFF);        /* pozycja w polowie okresu */
    int32_t y = (int32_t)(((int64_t)x * (32768 - x)) >> 13);
    if (y > 32767)
        y = 32767;
    return (phase & 0x80000000u) ? -y : y;
}

static int32_t wave_value(struct voice *v)
{
    switch (v->wave) {
    case WAVE_SQUARE:
        return (v->phase & 0x80000000u) ? -20000 : 20000;
    case WAVE_TRIANGLE: {
        int32_t t = (int32_t)(v->phase >> 16);
        return t < 32768 ? t * 2 - 32768 : 32767 - (t - 32768) * 2;
    }
    case WAVE_PIANO:
        return (sine(v->phase) * 3 + sine(v->phase * 2) + sine(v->phase * 3) / 2) / 4;
    case WAVE_NOISE:
        noise ^= noise << 13;
        noise ^= noise >> 17;
        noise ^= noise << 5;
        return (int32_t)(noise & 0xFFFF) - 32768;
    default:
        return sine(v->phase);
    }
}

/* Obwiednia: jak glosnosc zmienia sie w czasie trwania nuty */
static int32_t envelope(const struct voice *v)
{
    int32_t amp = v->volume;
    if (v->age < ATTACK)
        amp = (int32_t)((int64_t)amp * v->age / ATTACK);
    if (v->wave == WAVE_PIANO) {                          /* struna powoli wybrzmiewa */
        uint32_t d = AC97_RATE / 3;
        amp = (int32_t)((int64_t)amp * d / (d + v->age));
    }
    uint32_t left = v->length - v->age;
    if (left < RELEASE)
        amp = (int32_t)((int64_t)amp * left / RELEASE);
    return amp;
}

/* Wolane z przerwania karty: wypelnia kawalek dzwieku */
static void mix(int16_t *out, unsigned frames)
{
    for (unsigned f = 0; f < frames; f++) {
        int32_t l = 0, r = 0;
        for (int i = 0; i < VOICES; i++) {
            struct voice *v = &voices[i];
            if (!v->active)
                continue;
            int32_t s = (int32_t)(((int64_t)wave_value(v) * envelope(v)) >> 15);
            l += s;
            r += s;
            v->phase += v->step;
            if (++v->age >= v->length)
                v->active = 0;
        }
        if (pcm.active) {
            uint32_t i = (uint32_t)(pcm.pos >> 32);
            if (i >= pcm.frames) {
                pcm.active = 0;
            } else {
                /* Probka "pomiedzy" dwiema z nagrania (interpolacja liniowa) -
                 * inaczej np. 22050 Hz na karcie 48000 Hz brzmialoby "schodkowo" */
                uint32_t j = i + 1 < pcm.frames ? i + 1 : i;
                int32_t frac = (int32_t)((pcm.pos >> 16) & 0xFFFF);
                int32_t l0 = pcm.data[i * pcm.channels], l1 = pcm.data[j * pcm.channels];
                int32_t pl = l0 + (int32_t)(((int64_t)(l1 - l0) * frac) >> 16);
                int32_t pr = pl;
                if (pcm.channels == 2) {
                    int32_t r0 = pcm.data[i * 2 + 1], r1 = pcm.data[j * 2 + 1];
                    pr = r0 + (int32_t)(((int64_t)(r1 - r0) * frac) >> 16);
                }
                l += (pl * pcm.volume) >> 15;
                r += (pr * pcm.volume) >> 15;
                pcm.pos += pcm.step;
            }
        }
        /* Za glosno - przycinamy (inaczej liczba "przewinelaby sie" i trzasnelo) */
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        out[f * 2] = (int16_t)l;
        out[f * 2 + 1] = (int16_t)r;
    }
}

int audio_init(void)
{
    memset(voices, 0, sizeof(voices));
    available = ac97_init(mix);
    return available;
}

void audio_note(uint16_t freq, uint16_t ms, int wave, int volume)
{
    if (!available || freq < 20 || ms == 0)
        return;
    if (volume > 100)
        volume = 100;
    unsigned long flags = irq_save();
    int slot = 0;
    for (int i = 0; i < VOICES; i++) {                   /* wolny glos albo najstarszy */
        if (!voices[i].active) {
            slot = i;
            break;
        }
        if (voices[i].age > voices[slot].age)
            slot = i;
    }
    struct voice *v = &voices[slot];
    v->wave = wave;
    v->phase = 0;
    v->step = (uint32_t)(((uint64_t)freq << 32) / AC97_RATE);
    v->age = 0;
    v->length = (uint32_t)ms * (AC97_RATE / 1000);
    v->volume = volume * 32767 / 100 / 2;                /* polowa - miejsce na akordy */
    v->active = 1;
    irq_restore(flags);
}

void audio_play_pcm(const int16_t *data, uint32_t frames, int channels, uint32_t rate, int volume)
{
    if (!available)
        return;
    unsigned long flags = irq_save();
    pcm.data = data;
    pcm.frames = frames;
    pcm.channels = channels == 2 ? 2 : 1;
    pcm.pos = 0;
    pcm.step = ((uint64_t)rate << 32) / AC97_RATE;       /* np. 22050 Hz -> pol probki na krok */
    pcm.volume = volume * 32767 / 100;
    pcm.active = frames > 0;
    irq_restore(flags);
}

int audio_pcm_playing(void) { return pcm.active; }

uint32_t audio_pcm_position(void) { return (uint32_t)(pcm.pos >> 32); }

void audio_stop_all(void)
{
    unsigned long flags = irq_save();
    for (int i = 0; i < VOICES; i++)
        voices[i].active = 0;
    pcm.active = 0;
    irq_restore(flags);
}

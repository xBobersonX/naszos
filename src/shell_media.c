/* Komenda "graj" - odtwarza plik dzwiekowy WAV.
 *
 * WAV to najprostszy format dzwieku: naglowek ("RIFF....WAVE", potem
 * kawalek "fmt " z opisem - ile kanalow, ile probek na sekunde, ile bitow
 * na probke) i kawalek "data" z samymi probkami, bez zadnej kompresji. */

#include <stdint.h>
#include "shell_media.h"
#include "audio.h"
#include "fs.h"
#include "vga.h"
#include "heap.h"
#include "string.h"
#include "keyboard.h"
#include "thread.h"
#include "timer.h"

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

struct wav {
    int channels;
    uint32_t rate, frames;
    int16_t *samples;                    /* kmalloc - zawsze 16 bitow */
};

static const char *wav_load(const uint8_t *d, uint32_t size, struct wav *w)
{
    if (size < 12 || memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WAVE", 4) != 0)
        return "to nie jest plik WAV";
    const uint8_t *fmt = 0, *data = 0;
    uint32_t data_len = 0;
    for (uint32_t pos = 12; pos + 8 <= size;) {           /* kawalki: nazwa + dlugosc + tresc */
        uint32_t len = le32(d + pos + 4);
        if (len > size - pos - 8)
            len = size - pos - 8;
        if (memcmp(d + pos, "fmt ", 4) == 0 && len >= 16)
            fmt = d + pos + 8;
        else if (memcmp(d + pos, "data", 4) == 0) {
            data = d + pos + 8;
            data_len = len;
        }
        pos += 8 + len + (len & 1);
    }
    if (!fmt || !data)
        return "uszkodzony plik WAV";
    unsigned format = le16(fmt), bits = le16(fmt + 14);
    w->channels = le16(fmt + 2);
    w->rate = le32(fmt + 4);
    if (format != 1)
        return "skompresowany WAV - obslugujemy tylko zwykly PCM";
    if (w->channels < 1 || w->channels > 2 || (bits != 8 && bits != 16) || w->rate < 4000 || w->rate > 96000)
        return "nieobslugiwany rodzaj WAV (tylko mono/stereo, 8 lub 16 bitow)";
    unsigned bytes = bits / 8 * (unsigned)w->channels;
    w->frames = data_len / bytes;
    w->samples = kmalloc((size_t)w->frames * (unsigned)w->channels * 2 + 2);
    if (!w->samples)
        return "za malo pamieci";
    uint32_t n = w->frames * (uint32_t)w->channels;
    for (uint32_t i = 0; i < n; i++)
        w->samples[i] = bits == 16 ? (int16_t)le16(data + i * 2)
                                   : (int16_t)((data[i] - 128) << 8);   /* 8 bitow: 0..255, srodek 128 */
    return 0;
}

void cmd_graj(char *args)
{
    while (*args == ' ')
        args++;
    if (!*args) {
        vga_write("Uzycie: graj <plik.wav>   (plik mozna sciagnac komenda pobierz)\n");
        return;
    }
    if (!audio_available()) {
        vga_write("Brak karty dzwiekowej - uruchom NaszOS przez build.ps1 -Run.\n");
        return;
    }
    struct fs_node *f;
    struct fs_node *cwd = fs_node_valid(con_cur->cwd) ? con_cur->cwd : fs_root();
    if (fs_lookup(cwd, args, &f) != FS_OK || f->is_dir) {
        kprintf("Nie ma pliku %s\n", args);
        return;
    }
    struct wav w;
    const char *err = wav_load((const uint8_t *)f->data, f->size, &w);
    if (err) {
        kprintf("%s: %s\n", args, err);
        return;
    }
    uint32_t secs = w.frames / w.rate;
    kprintf("Gram %s: %u Hz, %s, %u:%02u  (Q albo Esc - stop)\n", args, w.rate,
            w.channels == 2 ? "stereo" : "mono", secs / 60, secs % 60);
    audio_play_pcm(w.samples, w.frames, w.channels, w.rate, 90);

    /* Pasek postepu */
    unsigned width = VGA_WIDTH > 20 ? VGA_WIDTH - 12 : 20, drawn = (unsigned)-1;
    unsigned x0, y;
    vga_get_pos(&x0, &y);
    while (audio_pcm_playing() && !con_cur->closed) {
        int k = keyboard_poll();
        if (k == 'q' || k == 'Q' || k == 27) {
            audio_stop_all();
            break;
        }
        uint32_t pos = audio_pcm_position();
        unsigned fill = w.frames ? (unsigned)((uint64_t)pos * width / w.frames) : 0;
        if (fill != drawn) {
            drawn = fill;
            uint32_t s = pos / w.rate;
            vga_set_pos(0, y);
            vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
            for (unsigned i = 0; i < width; i++)
                vga_putc(i < fill ? '\xDB' : '\xB0');
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            kprintf(" %u:%02u", s / 60, s % 60);
        }
        cpu_idle();
    }
    if (con_cur->closed)
        audio_stop_all();
    vga_putc('\n');
    kfree(w.samples);
}

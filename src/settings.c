/* Ustawienia pulpitu: motywy tapet i kolorow oraz zapis w pliku
 * /ustawienia.cfg (tekst "klucz=wartosc", jedna para na linie). */

#include "gui.h"
#include "fs.h"
#include "storage.h"
#include "string.h"
#include "speaker.h"

#define SETTINGS_FILE "/ustawienia.cfg"

const struct wallpaper gui_wallpapers[GUI_WALLPAPERS] = {
    { "Ocean",         0x0F2744, 0x2A5C7A },
    { "Zachod slonca", 0x3B0764, 0xEA580C },
    { "Las",           0x052E16, 0x4D7C0F },
    { "Noc",           0x020617, 0x312E81 },
    { "Grafit",        0x111827, 0x4B5563 },
    { "Malina",        0x500724, 0xDB2777 },
};

const struct accent gui_accents[GUI_ACCENTS] = {
    { "Niebieski",  0x2563EB, 0x1E40AF },
    { "Zielony",    0x16A34A, 0x166534 },
    { "Czerwony",   0xDC2626, 0x991B1B },
    { "Fioletowy",  0x9333EA, 0x6B21A8 },
    { "Pomarancz",  0xEA580C, 0x9A3412 },
    { "Grafitowy",  0x6B7280, 0x374151 },
};

static struct gui_settings settings = { .wallpaper = 0, .accent = 0, .watermark = 1, .seconds = 1, .sound = 1 };

struct gui_settings *gui_get_settings(void)
{
    return &settings;
}

uint32_t gui_accent_color(void) { return gui_accents[settings.accent].light; }
uint32_t gui_accent_dark(void)  { return gui_accents[settings.accent].dark; }

static int parse(const char *p, const char *end, int max, int fallback)
{
    int v = 0, digits = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        v = v * 10 + (*p++ - '0');
        digits++;
    }
    return digits && v >= 0 && v < max ? v : fallback;
}

void gui_settings_load(void)
{
    struct fs_node *f;
    if (fs_lookup(fs_root(), SETTINGS_FILE, &f) != FS_OK || f->is_dir)
        return;
    const char *p = f->data, *end = f->data + f->size;
    while (p < end) {
        const char *line = p;
        while (p < end && *p != '\n')
            p++;
        const char *eq = line;
        while (eq < p && *eq != '=')
            eq++;
        if (eq < p) {
            size_t klen = (size_t)(eq - line);
            const char *val = eq + 1;
            if (klen == 6 && memcmp(line, "tapeta", 6) == 0)
                settings.wallpaper = parse(val, p, GUI_WALLPAPERS, settings.wallpaper);
            else if (klen == 6 && memcmp(line, "akcent", 6) == 0)
                settings.accent = parse(val, p, GUI_ACCENTS, settings.accent);
            else if (klen == 5 && memcmp(line, "napis", 5) == 0)
                settings.watermark = parse(val, p, 2, settings.watermark);
            else if (klen == 7 && memcmp(line, "sekundy", 7) == 0)
                settings.seconds = parse(val, p, 2, settings.seconds);
            else if (klen == 6 && memcmp(line, "dzwiek", 6) == 0)
                settings.sound = parse(val, p, 2, settings.sound);
        }
        p++;                                    /* pomijamy '\n' */
    }
    sound_set_enabled(settings.sound);
}

void gui_settings_save(void)
{
    char buf[96];
    int n = ksnprintf(buf, sizeof(buf), "tapeta=%u\nakcent=%u\nnapis=%u\nsekundy=%u\ndzwiek=%u\n",
                      settings.wallpaper, settings.accent, settings.watermark, settings.seconds,
                      settings.sound);
    sound_set_enabled(settings.sound);
    struct fs_node *f;
    int err = fs_lookup(fs_root(), SETTINGS_FILE, &f);
    if (err == FS_ENOENT)
        err = fs_create(fs_root(), SETTINGS_FILE, 0, &f);
    if (err == FS_OK && fs_write(f, buf, (uint32_t)n, 0) == FS_OK)
        storage_sync_if_dirty();
}

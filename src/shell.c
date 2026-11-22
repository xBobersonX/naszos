/* Powloka: czyta linie z klawiatury, rozdziela nazwe komendy od argumentow
 * i wywoluje odpowiednia funkcje z tabeli commands[]. */

#include <stdint.h>
#include "shell.h"
#include "vga.h"
#include "keyboard.h"
#include "timer.h"
#include "string.h"
#include "io.h"
#include "kernel.h"
#include "pmm.h"
#include "heap.h"
#include "fs.h"
#include "shell_fs.h"
#include "shell_net.h"
#include "shell_media.h"
#include "storage.h"
#include "snake.h"
#include "rtc.h"
#include "mouse.h"
#include "power.h"
#include "gui.h"
#include "thread.h"
#include "speaker.h"
#include "process.h"

static int strchr_(const char *s, char c)
{
    for (; *s; s++)
        if (*s == c)
            return 1;
    return 0;
}

#define LINE_MAX 76

extern char _kernel_start[], _kernel_end[];   /* z linker.ld */

struct command {
    const char *name;
    const char *help;
    void (*run)(char *args);
};

static void cmd_help(char *args);
static void cmd_history(char *args);

static void cmd_clear(char *args)
{
    (void)args;
    vga_clear();
}

static void cmd_echo(char *args)
{
    kprintf("%s\n", args);
}

static void cmd_ver(char *args)
{
    (void)args;
    vga_write(OS_NAME " v" OS_VERSION " - wlasny system operacyjny, x86-64 (64 bity), jezyk C\n");
}

static void cmd_mem(char *args)
{
    (void)args;
    uint32_t total = pmm_total_frames(), used = pmm_used_frames();
    uint32_t kernel_kib = (uint32_t)(_kernel_end - _kernel_start + 1023) / 1024;
    struct heap_stats h;
    heap_get_stats(&h);

    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_write("Pamiec fizyczna\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("  Razem:   %6u KiB (%u ramek po 4 KiB)\n", total * 4, total);
    kprintf("  Zajete:  %6u KiB (%u ramek)\n", used * 4, used);
    kprintf("  Wolne:   %6u KiB (%u ramek)\n", (total - used) * 4, total - used);
    kprintf("  Kernel:  %6u KiB (0x%08lx - 0x%08lx)\n",
            kernel_kib, (uint64_t)_kernel_start, (uint64_t)_kernel_end);

    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_write("Sterta kernela (kmalloc)\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("  Rozmiar: %6u B, przydzielone %u B, wolne %u B\n", h.total, h.used, h.free);
    kprintf("  Bloki:   %6u (wolnych %u, najwiekszy wolny %u B)\n",
            h.blocks, h.free_blocks, h.largest_free);
}

static void cmd_memmap(char *args)
{
    (void)args;
    pmm_print_map();
}

static int pattern_ok(const uint8_t *p, uint32_t n, uint8_t v)
{
    for (uint32_t i = 0; i < n; i++)
        if (p[i] != v)
            return 0;
    return 1;
}

static void test_result(const char *name, int passed)
{
    kprintf("  %s ", name);
    for (size_t i = strlen(name); i < 52; i++)
        vga_putc('.');
    vga_set_color(passed ? VGA_LIGHT_GREEN : VGA_LIGHT_RED, VGA_BLACK);
    vga_write(passed ? " OK\n" : " BLAD\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* Test alokatorow: przydziela, zapisuje wzorce, sprawdza, ze bloki na siebie
 * nie nachodza, zwalnia i sprawdza, ze sterta wrocila do stanu poczatkowego. */
static void cmd_memtest(char *args)
{
    (void)args;
    enum { N = 64 };
    uint8_t *p[N];
    uint32_t sz[N];
    struct heap_stats before, mid, after;
    int ok;

    heap_get_stats(&before);
    uint32_t frames_before = pmm_used_frames();

    ok = 1;
    for (int i = 0; i < N; i++) {
        sz[i] = (uint32_t)(i * 97) % 2000 + 1;
        p[i] = kmalloc(sz[i]);
        if (!p[i] || (uintptr_t)p[i] % 16)
            ok = 0;
        else
            memset(p[i], i, sz[i]);
    }
    for (int i = 0; i < N && ok; i++)
        ok = pattern_ok(p[i], sz[i], (uint8_t)i);
    test_result("64 blokow 1-2000 B, wyrownanie, brak nakladania", ok);

    ok = 1;
    for (int i = 1; i < N; i += 2) {
        kfree(p[i]);
        sz[i] = sz[i] / 2 + 1;
        p[i] = kmalloc(sz[i]);
        if (!p[i])
            ok = 0;
        else
            memset(p[i], 0xA5, sz[i]);
    }
    for (int i = 0; i < N && ok; i++)
        ok = pattern_ok(p[i], sz[i], i % 2 ? 0xA5 : (uint8_t)i);
    test_result("zwolnienie co drugiego i ponowny przydzial", ok);

    uint8_t *big = kmalloc(1024 * 1024);
    ok = big != 0;
    if (ok) {
        memset(big, 0x5A, 1024 * 1024);
        ok = pattern_ok(big, 1024 * 1024, 0x5A);
        for (int i = 0; i < N && ok; i++)
            ok = pattern_ok(p[i], sz[i], i % 2 ? 0xA5 : (uint8_t)i);
    }
    heap_get_stats(&mid);
    /* Sterta rosnie tylko wtedy, gdy nie ma juz wolnego 1 MiB (np. po poprzednim tescie jest) */
    test_result(mid.total > before.total ? "blok 1 MiB (sterta urosla)"
                                         : "blok 1 MiB (z wolnego miejsca w stercie)", ok);

    kfree(big);
    for (int i = 0; i < N; i++)
        kfree(p[i]);
    heap_get_stats(&after);
    test_result("po zwolnieniu wszystko wolne i sklejone",
                after.used == before.used && after.free_blocks <= before.free_blocks);

    uintptr_t f1 = pmm_alloc(), f2 = pmm_alloc(), f3 = pmm_alloc();
    ok = f1 && f2 && f3 && f1 != f2 && f2 != f3 && f1 != f3 &&
         f1 % FRAME_SIZE == 0 && f2 % FRAME_SIZE == 0 && f3 % FRAME_SIZE == 0;
    if (f1) pmm_free(f1);
    if (f2) pmm_free(f2);
    if (f3) pmm_free(f3);
    test_result("PMM: 3 rozne ramki, wyrownane do 4 KiB, zwolnione",
                ok && pmm_used_frames() == frames_before + (mid.total - before.total) / FRAME_SIZE);

    kprintf("Sterta: %u KiB -> %u KiB (sterta nie oddaje pamieci do PMM)\n",
            before.total / 1024, after.total / 1024);
}

static void cmd_time(char *args)
{
    (void)args;
    uint32_t ms = timer_uptime_ms();
    uint32_t s = ms / 1000;
    kprintf("Czas pracy: %u h %u min %u s (%u tykniec zegara)\n",
            s / 3600, s / 60 % 60, s % 60, timer_ticks());
}

static void cmd_reboot(char *args)
{
    (void)args;
    system_reboot();
}

static void cmd_shutdown(char *args)
{
    (void)args;
    system_shutdown();
}

static void print_storage_error(int err)
{
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    kprintf("Blad zapisu na dysk: %s\n", storage_strerror(err));
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void cmd_sync(char *args)
{
    (void)args;
    int err = storage_save();
    if (err)
        print_storage_error(err);
    else
        vga_write("Zapisano wszystkie pliki na dysk.\n");
}

static void cmd_disk(char *args)
{
    (void)args;
    storage_print_info();
}

static void cmd_date(char *args)
{
    (void)args;
    struct rtc_time t;
    rtc_read(&t);
    kprintf("%s, %u %s %u, %02u:%02u:%02u\n", rtc_day_name[t.weekday], t.day,
            rtc_month_genitive[t.month - 1], t.year, t.hour, t.minute, t.second);
}

/* Czyta liczbe dziesietna z *s; -1 gdy nie ma liczby */
static int parse_uint(char **s)
{
    while (**s == ' ')
        (*s)++;
    if (**s < '0' || **s > '9')
        return -1;
    int v = 0;
    while (**s >= '0' && **s <= '9' && v < 100000)
        v = v * 10 + (*(*s)++ - '0');
    return v;
}

static void cmd_cal(char *args)
{
    struct rtc_time today;
    rtc_read(&today);
    int month = today.month, year = today.year;

    if (*args) {
        month = parse_uint(&args);
        year = parse_uint(&args);
        if (month < 1 || month > 12 || year < 1 || year > 9999) {
            vga_write("Uzycie: cal [miesiac rok], np. cal 12 2026\n");
            return;
        }
    }

    /* Naglowek wysrodkowany nad 20 znakami tabelki */
    char title[24];
    const char *name = rtc_month_name[month - 1];
    size_t tlen = strlen(name) + 5;
    size_t pad = tlen < 20 ? (20 - tlen) / 2 : 0;
    for (size_t i = 0; i < pad; i++)
        title[i] = ' ';
    title[pad] = '\0';
    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("%s%s %u\n", title, name, year);
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_write("Pn Wt Sr Cz Pt So Nd\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    int first = (rtc_weekday(year, month, 1) + 6) % 7;   /* tydzien od poniedzialku */
    int days = rtc_days_in_month(year, month);
    for (int i = 0; i < first; i++)
        vga_write("   ");
    for (int d = 1; d <= days; d++) {
        int col = (first + d - 1) % 7;
        int is_today = d == today.day && month == today.month && year == today.year;
        if (is_today)
            vga_set_color(VGA_BLACK, VGA_YELLOW);
        else if (col >= 5)
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);     /* weekend */
        kprintf("%2u", d);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        vga_putc(col == 6 || d == days ? '\n' : ' ');
    }
}

/* beep [czestotliwosc Hz] [czas ms], np. beep 880 300 */
static void cmd_beep(char *args)
{
    int freq = parse_uint(&args), ms = parse_uint(&args);
    if (freq < 0)
        freq = 880;
    if (ms < 0)
        ms = 200;
    if (freq < 20 || freq > 20000 || ms > 5000) {
        vga_write("Uzycie: beep [20-20000 Hz] [do 5000 ms], np. beep 440 500\n");
        return;
    }
    if (!sound_enabled()) {
        vga_write("Dzwieki sa wylaczone (Ustawienia na pulpicie).\n");
        return;
    }
    sound_beep((uint16_t)freq, (uint16_t)ms);
    sound_wait();
}

static void cmd_melodia(char *args)
{
    (void)args;
    if (!sound_enabled()) {
        vga_write("Dzwieki sa wylaczone (Ustawienia na pulpicie).\n");
        return;
    }
    vga_write("Gram: \"Wlazl kotek na plotek\"...\n");
    sound_melody();
    sound_wait();
}

/* Szuka programu: podana sciezka, nazwa w biezacym katalogu albo w /programy,
 * z koncowka .exe albo bez niej */
static int find_program(const char *name, char *path, size_t size)
{
    struct fs_node *cwd = fs_node_valid(con_cur->cwd) ? con_cur->cwd : fs_root();
    const char *fmt[4] = { "%s", "%s.exe", "/programy/%s", "/programy/%s.exe" };
    for (int i = 0; i < 4; i++) {
        if (i >= 2 && strchr_(name, '/'))
            break;                              /* sciezki nie szukamy w /programy */
        ksnprintf(path, size, fmt[i], name);
        struct fs_node *f;
        if (fs_lookup(cwd, path, &f) == FS_OK && !f->is_dir &&
            f->size >= 4 && memcmp(f->data, "\x7F" "ELF", 4) == 0)
            return 1;
    }
    return 0;
}

static void cmd_run(char *args)
{
    char path[FS_PATH_MAX];
    if (!*args) {
        vga_write("Uzycie: run <program>, np. run kalkulator  (programy sa w /programy)\n");
        return;
    }
    if (!find_program(args, path, sizeof(path))) {
        kprintf("run: nie znaleziono programu '%s' (zobacz: ls /programy)\n", args);
        return;
    }
    process_run_and_wait(path);
}

static void cmd_snake(char *args)
{
    (void)args;
    snake_run();
}

static void cmd_pulpit(char *args)
{
    (void)args;
    if (!thread_is_main()) {
        vga_write("Pulpit juz dziala - to jest okno terminala na pulpicie.\n");
        return;
    }
    if (!gui_run()) {
        vga_write("Pulpit wymaga trybu graficznego - uruchom system przez\n"
                  ".\\build.ps1 -Run (pelny ekran) albo .\\build.ps1 -Run -Okno.\n");
        return;
    }
    vga_write("Wpisz 'pulpit', aby wrocic na pulpit.\n");
}

static void cmd_panic(char *args)
{
    (void)args;
    vga_write("Wywoluje dzielenie przez zero...\n");
    __asm__ volatile ("xor %%ecx, %%ecx; div %%ecx" ::: "eax", "ecx", "edx");
}

/* Opisy maja max 27 znakow - help wypisuje je w dwoch kolumnach */
static const struct command commands[] = {
    { "help",     "lista komend",                cmd_help },
    { "clear",    "czysci ekran",                cmd_clear },
    { "echo",     "wypisuje tekst",              cmd_echo },
    { "ls",       "zawartosc katalogu",          cmd_ls },
    { "cd",       "zmienia katalog (cd ..)",     cmd_cd },
    { "pwd",      "biezacy katalog",             cmd_pwd },
    { "mkdir",    "tworzy katalog",              cmd_mkdir },
    { "touch",    "tworzy pusty plik",           cmd_touch },
    { "write",    "write plik tekst",            cmd_write },
    { "append",   "append plik tekst",           cmd_append },
    { "cat",      "wyswietla plik",              cmd_cat },
    { "edit",     "edytor tekstu",               cmd_edit },
    { "rm",       "usuwa plik/pusty katalog",    cmd_rm },
    { "ver",      "wersja systemu",              cmd_ver },
    { "mem",      "uzycie pamieci i sterty",     cmd_mem },
    { "memmap",   "mapa pamieci od BIOS-u",      cmd_memmap },
    { "memtest",  "test alokatorow pamieci",     cmd_memtest },
    { "disk",     "informacje o dysku",          cmd_disk },
    { "sync",     "zapisz pliki na dysk teraz",  cmd_sync },
    { "date",     "data i godzina",              cmd_date },
    { "cal",      "kalendarz, np. cal 12 2026",  cmd_cal },
    { "time",     "czas od uruchomienia",        cmd_time },
    { "history",  "ostatnie komendy",            cmd_history },
    { "snake",    "gra w weza",                  cmd_snake },
    { "beep",     "dzwiek, np. beep 440 500",    cmd_beep },
    { "melodia",  "zagraj melodie",              cmd_melodia },
    { "run",      "uruchom program .exe",        cmd_run },
    { "siec",     "ustawienia sieci (IP, MAC)",  cmd_siec },
    { "ping",     "ping google.com",             cmd_ping },
    { "dns",      "adres IP dla nazwy",          cmd_dns },
    { "www",      "pobierz strone: www adres",   cmd_www },
    { "czas",     "ustaw zegar z internetu",     cmd_czas },
    { "pobierz",  "sciagnij plik z internetu",   cmd_pobierz },
    { "js",       "JavaScript: js 2+2, js plik.js", cmd_js },
    { "graj",     "odtworz plik .wav",           cmd_graj },
    { "kryptotest", "test szyfrowania (HTTPS)",  cmd_kryptotest },
    { "pulpit",   "pulpit z oknami (myszka)",    cmd_pulpit },
    { "reboot",   "restart komputera",           cmd_reboot },
    { "shutdown", "wylacza komputer",            cmd_shutdown },
    { "panic",    "test bledu krytycznego",      cmd_panic },
    { 0, 0, 0 },
};

static void print_help_entry(const struct command *c)
{
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("  %s", c->name);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    for (size_t i = strlen(c->name); i < 10; i++)
        vga_putc(' ');
    vga_write(c->help);
}

static void cmd_help(char *args)
{
    (void)args;
    size_t count = 0;
    while (commands[count].name)
        count++;
    size_t rows = (count + 1) / 2;

    vga_write("Dostepne komendy (strzalki gora/dol = historia):\n");
    for (size_t r = 0; r < rows; r++) {
        print_help_entry(&commands[r]);
        if (r + rows < count) {
            for (size_t i = 12 + strlen(commands[r].help); i < 40; i++)
                vga_putc(' ');
            print_help_entry(&commands[r + rows]);
        }
        vga_putc('\n');
    }
}

/* ---- Historia komend: bufor cykliczny HISTORY_MAX ostatnich linii ---- */

#define HISTORY_MAX 16

static char history[HISTORY_MAX][LINE_MAX + 1];
static uint32_t history_count;           /* ile linii dodano od startu */

static void history_add(const char *line)
{
    if (!*line)
        return;
    if (history_count > 0 &&
        strcmp(history[(history_count - 1) % HISTORY_MAX], line) == 0)
        return;                          /* nie powtarzamy tej samej komendy pod rzad */
    strcpy(history[history_count % HISTORY_MAX], line);
    history_count++;
}

/* n = 1 to ostatnia komenda, 2 - przedostatnia itd. */
static const char *history_get(uint32_t n)
{
    uint32_t stored = history_count < HISTORY_MAX ? history_count : HISTORY_MAX;
    if (n == 0 || n > stored)
        return 0;
    return history[(history_count - n) % HISTORY_MAX];
}

static void cmd_history(char *args)
{
    (void)args;
    uint32_t stored = history_count < HISTORY_MAX ? history_count : HISTORY_MAX;
    for (uint32_t n = stored; n > 0; n--)
        kprintf("%4u  %s\n", history_count - n + 1, history_get(n));
}

/* ---- Edycja linii ----
 * Linia zawsze miesci sie w jednym wierszu ekranu (limit dlugosci zalezy od
 * dlugosci znaku zachety), wiec po kazdej zmianie mozna ja po prostu
 * przerysowac od miejsca, w ktorym sie zaczyna. */

struct line_editor {
    char *buf;
    size_t len, pos, max;
    unsigned x0, y0;                     /* gdzie na ekranie zaczyna sie linia */
};

static void redraw(struct line_editor *e, size_t old_len)
{
    vga_set_pos(e->x0, e->y0);
    for (size_t i = 0; i < e->len; i++)
        vga_putc(e->buf[i]);
    for (size_t i = e->len; i < old_len; i++)
        vga_putc(' ');                   /* zamazujemy resztki dluzszej linii */
    vga_set_pos(e->x0 + (unsigned)e->pos, e->y0);
}

static void set_text(struct line_editor *e, const char *text)
{
    size_t old_len = e->len;
    e->len = 0;
    while (text[e->len] && e->len < e->max) {
        e->buf[e->len] = text[e->len];
        e->len++;
    }
    e->pos = e->len;
    redraw(e, old_len);
}

static void read_line(char *buf)
{
    struct line_editor e = { .buf = buf };
    char draft[LINE_MAX + 1];            /* niedokonczona linia sprzed wejscia w historie */
    uint32_t hist_pos = 0;               /* 0 = edytujemy nowa linie */

    vga_get_pos(&e.x0, &e.y0);
    if (e.x0 + 16 >= vga_width()) {      /* bardzo dlugi znak zachety - piszemy nizej */
        vga_write("\n> ");
        vga_get_pos(&e.x0, &e.y0);
    }
    e.max = vga_width() - 1 - e.x0;
    if (e.max > LINE_MAX)
        e.max = LINE_MAX;

    for (;;) {
        int key = keyboard_getkey();
        size_t old_len = e.len;

        if (con_cur->closed) {           /* okno terminala zamkniete */
            buf[0] = '\0';
            return;
        }

        switch (key) {
        case '\n':
            buf[e.len] = '\0';
            vga_set_pos(e.x0 + (unsigned)e.len, e.y0);
            vga_putc('\n');
            return;

        case '\b':
            if (e.pos == 0)
                break;
            memmove(buf + e.pos - 1, buf + e.pos, e.len - e.pos);
            e.pos--;
            e.len--;
            redraw(&e, old_len);
            break;

        case KEY_DELETE:
            if (e.pos == e.len)
                break;
            memmove(buf + e.pos, buf + e.pos + 1, e.len - e.pos - 1);
            e.len--;
            redraw(&e, old_len);
            break;

        case KEY_LEFT:  if (e.pos > 0)     e.pos--; redraw(&e, old_len); break;
        case KEY_RIGHT: if (e.pos < e.len) e.pos++; redraw(&e, old_len); break;
        case KEY_HOME:  e.pos = 0;                  redraw(&e, old_len); break;

        case KEY_MOUSE_LEFT: {                  /* klikniecie w pisana linie ustawia kursor */
            unsigned mx, my;
            mouse_last_click(&mx, &my);
            if (my == e.y0 && mx >= e.x0) {
                e.pos = mx - e.x0 < e.len ? mx - e.x0 : e.len;
                redraw(&e, old_len);
            }
            break;
        }
        case KEY_END:   e.pos = e.len;              redraw(&e, old_len); break;

        case KEY_UP:
            if (!history_get(hist_pos + 1))
                break;
            if (hist_pos == 0) {
                memcpy(draft, buf, e.len);
                draft[e.len] = '\0';
            }
            set_text(&e, history_get(++hist_pos));
            break;

        case KEY_DOWN:
            if (hist_pos == 0)
                break;
            hist_pos--;
            set_text(&e, hist_pos ? history_get(hist_pos) : draft);
            break;

        default:
            if (key < ' ' || key >= 127 || e.len >= e.max)
                break;
            memmove(buf + e.pos + 1, buf + e.pos, e.len - e.pos);
            buf[e.pos++] = (char)key;
            e.len++;
            redraw(&e, old_len);
            break;
        }
    }
}

void shell_read_line(char *buf)
{
    read_line(buf);
}

static void execute(char *line)
{
    while (*line == ' ')
        line++;
    if (!*line)
        return;

    char *args = line;
    while (*args && *args != ' ')
        args++;
    if (*args)
        *args++ = '\0';
    while (*args == ' ')
        args++;

    for (const struct command *c = commands; c->name; c++) {
        if (strcmp(line, c->name) == 0) {
            c->run(args);
            return;
        }
    }

    /* Nie ma takiej komendy - moze to program: "kalkulator" -> /programy/kalkulator.exe */
    char path[FS_PATH_MAX];
    if (find_program(line, path, sizeof(path))) {
        process_run_and_wait(path);
        return;
    }

    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    kprintf("Nieznana komenda: %s", line);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_write(" - wpisz 'help', aby zobaczyc liste.\n");
}

/* Petla powloki - konczy sie dopiero, gdy konsola zostanie zamknieta
 * (okno terminala); na ekranie trwa w nieskonczonosc */
static void shell_loop(void)
{
    char line[LINE_MAX + 1];
    char cwd[FS_PATH_MAX];

    while (!con_cur->closed) {
        shell_cwd_path(cwd, sizeof(cwd));
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        vga_write("naszos");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        vga_write(":");
        vga_set_color(VGA_LIGHT_BLUE, VGA_BLACK);
        vga_write(cwd);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        vga_write("> ");
        vga_set_color(VGA_WHITE, VGA_BLACK);
        read_line(line);
        if (con_cur->closed)
            return;
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        history_add(line);               /* przed execute - ono modyfikuje linie */
        execute(line);

        int err = storage_sync_if_dirty();   /* komenda cos zmienila - zapisujemy na dysk */
        if (err)
            print_storage_error(err);
    }
}

void shell_run(void)
{
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_write("\nWitaj w NaszOS! Wpisz 'help', aby zobaczyc liste komend.\n\n");

    if (vga_is_graphics()) {             /* w trybie graficznym startujemy od pulpitu */
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        cmd_pulpit("");
    }
    for (;;)
        shell_loop();
}

void shell_terminal(void *arg)
{
    (void)arg;
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_write(OS_NAME " v" OS_VERSION " - terminal\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_write("Wpisz 'help', aby zobaczyc komendy. Okno zamyka czerwony X.\n\n");
    shell_loop();
}

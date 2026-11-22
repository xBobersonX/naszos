/* Programy w oknach pulpitu: O systemie, Kalendarz, Pliki, Notatnik, Terminal.
 * (Snake w oknie jest w gsnake.c.) */

#include "gui.h"
#include "gfx.h"
#include "vga.h"
#include "fs.h"
#include "rtc.h"
#include "timer.h"
#include "pmm.h"
#include "heap.h"
#include "keyboard.h"
#include "editor.h"
#include "thread.h"
#include "string.h"
#include "kernel.h"
#include "speaker.h"
#include "process.h"
#include "mouse.h"

#define TEXT_DARK   0x111827
#define TEXT_MUTED  0x6B7280
#define ACCENT      gui_accent_color()
#define LINE_H      18

/* Obszar o stalym rozmiarze w srodku (byc moze powiekszonego) okna */
static struct rect centered(struct rect c, int w, int h)
{
    struct rect r = { c.x + (c.w > w ? (c.w - w) / 2 : 0), c.y + (c.h > h ? (c.h - h) / 2 : 0), w, h };
    return r;
}

static void button(struct rect r, const char *label)
{
    gfx_gradient(r.x, r.y, r.w, r.h, 0xFFFFFF, 0xD1D5DB);
    gfx_frame(r.x, r.y, r.w, r.h, 0x6B7280);
    gfx_text(r.x + (r.w - gfx_text_width(label)) / 2, r.y + (r.h - 16) / 2, label, TEXT_DARK);
}

static void free_state(struct window *w)
{
    kfree(w->state);
}

/* ================= Ustawienia ================= */

#define SET_W      560
#define SET_H      496
#define TILE_W     160
#define TILE_H     70
#define SWATCH_W   76
#define SWATCH_H   34

static struct rect tile_rect(struct rect a, int i)
{
    struct rect r = { a.x + 20 + (i % 3) * (TILE_W + 16), a.y + 84 + (i / 3) * (TILE_H + 30), TILE_W, TILE_H };
    return r;
}

static struct rect swatch_rect(struct rect a, int i)
{
    struct rect r = { a.x + 20 + i * (SWATCH_W + 10), a.y + 318, SWATCH_W, SWATCH_H };
    return r;
}

static struct rect check_rect(struct rect a, int i)
{
    struct rect r = { a.x + 20, a.y + 384 + i * 26, 300, 20 };
    return r;
}

static struct rect defaults_rect(struct rect a)
{
    struct rect r = { a.x + a.w - 150, a.y + 464, 130, 24 };
    return r;
}

static void selection_frame(struct rect r)
{
    for (int k = 1; k <= 3; k++)
        gfx_frame(r.x - k, r.y - k, r.w + 2 * k, r.h + 2 * k, gui_accent_color());
}

static void settings_draw(struct window *w, struct rect c)
{
    (void)w;
    struct gui_settings *st = gui_get_settings();
    struct rect a = centered(c, SET_W, SET_H);

    gfx_text_scaled(a.x + 20, a.y + 14, "Wyglad pulpitu", TEXT_DARK, 2);
    gfx_text(a.x + 20, a.y + 60, "Tapeta", TEXT_MUTED);
    for (int i = 0; i < GUI_WALLPAPERS; i++) {
        struct rect t = tile_rect(a, i);
        const struct wallpaper *wp = &gui_wallpapers[i];
        gfx_gradient(t.x, t.y, t.w, t.h, wp->top, wp->bottom);
        gfx_fill(t.x, t.y + t.h - 8, t.w, 8, 0x111827);            /* miniaturka paska zadan */
        gfx_fill(t.x + 44, t.y + 14, 72, 38, 0xF3F4F6);            /* i okna */
        gfx_fill(t.x + 44, t.y + 14, 72, 8, gui_accent_color());
        if (i == st->wallpaper)
            selection_frame(t);
        gfx_text(t.x + (t.w - gfx_text_width(wp->name)) / 2, t.y + t.h + 6, wp->name, TEXT_DARK);
    }

    gfx_text(a.x + 20, a.y + 294, "Kolor okien", TEXT_MUTED);
    for (int i = 0; i < GUI_ACCENTS; i++) {
        struct rect r = swatch_rect(a, i);
        gfx_gradient(r.x, r.y, r.w, r.h, gui_accents[i].light, gui_accents[i].dark);
        if (i == st->accent)
            selection_frame(r);
        gfx_text(r.x + (r.w - gfx_text_width(gui_accents[i].name)) / 2, r.y + r.h + 6,
                 gui_accents[i].name, TEXT_DARK);
    }

    const char *labels[3] = { "Napis NaszOS na tapecie", "Sekundy w zegarze na pasku zadan",
                              "Dzwieki (glosniczek PC)" };
    int values[3] = { st->watermark, st->seconds, st->sound };
    for (int i = 0; i < 3; i++) {
        struct rect r = check_rect(a, i);
        gfx_fill(r.x, r.y + 2, 16, 16, 0xFFFFFF);
        gfx_frame(r.x, r.y + 2, 16, 16, 0x6B7280);
        if (values[i]) {                                            /* "ptaszek" */
            gfx_fill(r.x + 3, r.y + 5, 10, 10, gui_accent_color());
            gfx_fill(r.x + 5, r.y + 9, 2, 3, 0xFFFFFF);
            gfx_fill(r.x + 7, r.y + 10, 2, 2, 0xFFFFFF);
            gfx_fill(r.x + 9, r.y + 7, 2, 3, 0xFFFFFF);
        }
        gfx_text(r.x + 26, r.y + 2, labels[i], TEXT_DARK);
    }

    gfx_text(a.x + 20, a.y + 470, "Zmiany zapisuja sie od razu na dysku.", TEXT_MUTED);
    button(defaults_rect(a), "Domyslne");
}

static void settings_click(struct window *w, int x, int y, int dbl)
{
    (void)dbl;
    struct gui_settings *st = gui_get_settings();
    struct rect a = centered(gui_client_size(w), SET_W, SET_H);
    int changed = 0;

    for (int i = 0; i < GUI_WALLPAPERS; i++) {
        struct rect t = tile_rect(a, i);
        t.h += 24;                                                   /* razem z podpisem */
        if (rect_contains(&t, x, y) && st->wallpaper != i) {
            st->wallpaper = i;
            changed = 1;
        }
    }
    for (int i = 0; i < GUI_ACCENTS; i++) {
        struct rect r = swatch_rect(a, i);
        r.h += 24;
        if (rect_contains(&r, x, y) && st->accent != i) {
            st->accent = i;
            changed = 1;
        }
    }
    struct rect c0 = check_rect(a, 0), c1 = check_rect(a, 1), c2 = check_rect(a, 2);
    struct rect d = defaults_rect(a);
    if (rect_contains(&c0, x, y)) {
        st->watermark = !st->watermark;
        changed = 1;
    }
    if (rect_contains(&c1, x, y)) {
        st->seconds = !st->seconds;
        changed = 1;
    }
    if (rect_contains(&c2, x, y)) {
        st->sound = !st->sound;
        changed = 1;
        if (st->sound) {
            sound_set_enabled(1);
            sound_beep(880, 120);                                    /* probka */
        }
    }
    if (rect_contains(&d, x, y)) {
        st->wallpaper = st->accent = 0;
        st->watermark = st->seconds = st->sound = 1;
        changed = 1;
    }
    if (changed)
        gui_apply_settings();                /* nowa tapeta/kolory na calym pulpicie + zapis */
}

static const struct app settings_app = {
    .draw = settings_draw, .click = settings_click, .min_w = SET_W, .min_h = SET_H,
};

void app_open_settings(void)
{
    gui_open(&settings_app, "Ustawienia", SET_W, SET_H, 0);
}

/* ================= O systemie ================= */

#define ABOUT_W 420
#define ABOUT_H 210

static void about_draw(struct window *w, struct rect c)
{
    (void)w;
    c = centered(c, ABOUT_W, ABOUT_H);
    char line[80];
    int x = c.x + 20, y = c.y + 16;
    unsigned sw = (unsigned)gfx_width(), sh = (unsigned)gfx_height();
    uint32_t s = timer_uptime_ms() / 1000;
    uint32_t total = pmm_total_frames() * 4 / 1024, free = (pmm_total_frames() - pmm_used_frames()) * 4 / 1024;

    gfx_text_scaled(x, y, OS_NAME, ACCENT, 3);
    gfx_text(x + gfx_text_width(OS_NAME) * 3 + 12, y + 26, "wersja " OS_VERSION, TEXT_MUTED);
    y += 64;
    gfx_fill(c.x + 16, y - 10, c.w - 32, 1, 0xD1D5DB);

    ksnprintf(line, sizeof(line), "Ekran:       %ux%u pikseli", sw, sh);
    gfx_text(x, y, line, TEXT_DARK);
    ksnprintf(line, sizeof(line), "Pamiec RAM:  %u MiB (wolne %u MiB)", total, free);
    gfx_text(x, y += LINE_H + 4, line, TEXT_DARK);
    ksnprintf(line, sizeof(line), "Czas pracy:  %u h %02u min %02u s", s / 3600, s / 60 % 60, s % 60);
    gfx_text(x, y += LINE_H + 4, line, TEXT_DARK);
    gfx_text(x, y += LINE_H + 4, "Procesor:    x86-64, tryb 64-bitowy", TEXT_DARK);
    gfx_text(x, y += LINE_H + 4, "Napisany w:  C i asemblerze", TEXT_DARK);
}

static const struct app about_app = { .draw = about_draw, .live = 1, .min_w = ABOUT_W, .min_h = ABOUT_H };

void app_open_about(void)
{
    gui_open(&about_app, "O systemie", ABOUT_W, ABOUT_H, 0);
}

/* ================= Kalendarz ================= */

struct calendar {
    int view_month, view_year;          /* pokazywany miesiac */
    int sel_day, sel_month, sel_year;   /* wybrany dzien */
};

#define CAL_W        434
#define CAL_H        476
#define CAL_HEADER   96
#define CAL_NAV_Y    (CAL_HEADER + 12)
#define CAL_GRID_X   14
#define CAL_GRID_Y   (CAL_HEADER + 76)
#define CAL_CELL_W   58
#define CAL_CELL_H   40
#define CAL_FOOTER_Y (CAL_GRID_Y + 6 * CAL_CELL_H + 12)

/* Numer dnia od 1970-01-01 (algorytm H. Hinnanta) - do liczenia roznicy dat */
static int32_t day_number(int y, int m, int d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void add_days(int *y, int *m, int *d, int n)
{
    *d += n;
    while (*d < 1) {
        if (--*m < 1) {
            *m = 12;
            --*y;
        }
        *d += rtc_days_in_month(*y, *m);
    }
    while (*d > rtc_days_in_month(*y, *m)) {
        *d -= rtc_days_in_month(*y, *m);
        if (++*m > 12) {
            *m = 1;
            ++*y;
        }
    }
}

static void add_months(int *y, int *m, int n)
{
    int total = *y * 12 + (*m - 1) + n;
    *y = total / 12;
    *m = total % 12 + 1;
}

/* Data w polu nr i siatki (0..41; tydzien od poniedzialku) */
static void cell_date(const struct calendar *cal, int i, int *y, int *m, int *d)
{
    int first = (rtc_weekday(cal->view_year, cal->view_month, 1) + 6) % 7;
    *y = cal->view_year;
    *m = cal->view_month;
    *d = 1;
    add_days(y, m, d, i - first);
}

static struct rect cal_btn_prev(struct rect c)  { struct rect r = { c.x + 14, c.y + CAL_NAV_Y, 30, 28 }; return r; }
static struct rect cal_btn_next(struct rect c)  { struct rect r = { c.x + 50, c.y + CAL_NAV_Y, 30, 28 }; return r; }
static struct rect cal_btn_today(struct rect c) { struct rect r = { c.x + c.w - 78, c.y + CAL_NAV_Y, 64, 28 }; return r; }

static void calendar_draw(struct window *w, struct rect c)
{
    struct calendar *cal = w->state;
    c = centered(c, CAL_W, CAL_H);
    struct rtc_time now;
    rtc_read(&now);
    char buf[64];

    /* Naglowek: duzy zegar i pelna data */
    gfx_gradient(c.x, c.y, c.w, CAL_HEADER, gui_accent_color(), gui_accent_dark());
    ksnprintf(buf, sizeof(buf), "%02u:%02u:%02u", now.hour, now.minute, now.second);
    gfx_text_scaled(c.x + 20, c.y + 12, buf, 0xFFFFFF, 3);
    ksnprintf(buf, sizeof(buf), "%s, %u %s %u", rtc_day_name[now.weekday], now.day,
              rtc_month_genitive[now.month - 1], now.year);
    gfx_text(c.x + 22, c.y + 68, buf, 0xBFDBFE);

    /* Nawigacja: < > Wrzesien 2026 [Dzis] */
    button(cal_btn_prev(c), "<");
    button(cal_btn_next(c), ">");
    button(cal_btn_today(c), "Dzis");
    ksnprintf(buf, sizeof(buf), "%s %u", rtc_month_name[cal->view_month - 1], cal->view_year);
    gfx_text_scaled(c.x + 94, c.y + CAL_NAV_Y - 2, buf, TEXT_DARK, 2);

    static const char *const days[7] = { "Pn", "Wt", "Sr", "Cz", "Pt", "So", "Nd" };
    int gx = c.x + CAL_GRID_X, gy = c.y + CAL_GRID_Y;
    for (int d = 0; d < 7; d++)
        gfx_text(gx + d * CAL_CELL_W + (CAL_CELL_W - 16) / 2, gy - 24, days[d],
                 d >= 5 ? 0xDC2626 : TEXT_MUTED);
    gfx_fill(gx, gy - 4, 7 * CAL_CELL_W, 1, 0xD1D5DB);

    for (int i = 0; i < 42; i++) {
        int y, m, d;
        cell_date(cal, i, &y, &m, &d);
        int col = i % 7, row = i / 7;
        int x0 = gx + col * CAL_CELL_W, y0 = gy + row * CAL_CELL_H;
        int other = m != cal->view_month;
        int today = d == now.day && m == now.month && y == now.year;
        int selected = d == cal->sel_day && m == cal->sel_month && y == cal->sel_year;

        if (today) {                                    /* niebieskie "kolko" */
            gfx_fill(x0 + 11, y0 + 4, CAL_CELL_W - 22, CAL_CELL_H - 8, ACCENT);
            gfx_fill(x0 + 8, y0 + 7, CAL_CELL_W - 16, CAL_CELL_H - 14, ACCENT);
        }
        if (selected && !today) {
            gfx_frame(x0 + 6, y0 + 3, CAL_CELL_W - 12, CAL_CELL_H - 6, ACCENT);
            gfx_frame(x0 + 7, y0 + 4, CAL_CELL_W - 14, CAL_CELL_H - 8, ACCENT);
        }
        uint32_t color = today ? 0xFFFFFF : other ? 0xC0C4CC : col >= 5 ? 0xDC2626 : TEXT_DARK;
        ksnprintf(buf, sizeof(buf), "%u", d);
        gfx_text(x0 + (CAL_CELL_W - gfx_text_width(buf)) / 2, y0 + (CAL_CELL_H - 16) / 2, buf, color);
    }

    /* Stopka: wybrany dzien i odleglosc od dzisiaj */
    gfx_fill(c.x, c.y + CAL_FOOTER_Y - 6, c.w, 1, 0xD1D5DB);
    int32_t diff = day_number(cal->sel_year, cal->sel_month, cal->sel_day) -
                   day_number(now.year, now.month, now.day);
    char when[32];
    if (diff == 0)
        ksnprintf(when, sizeof(when), "dzisiaj");
    else if (diff == 1)
        ksnprintf(when, sizeof(when), "jutro");
    else if (diff == -1)
        ksnprintf(when, sizeof(when), "wczoraj");
    else if (diff > 0)
        ksnprintf(when, sizeof(when), "za %d dni", diff);
    else
        ksnprintf(when, sizeof(when), "%d dni temu", -diff);
    ksnprintf(buf, sizeof(buf), "%s, %u %s %u",
              rtc_day_name[rtc_weekday(cal->sel_year, cal->sel_month, cal->sel_day)],
              cal->sel_day, rtc_month_genitive[cal->sel_month - 1], cal->sel_year);
    gfx_text(c.x + 16, c.y + CAL_FOOTER_Y + 4, buf, TEXT_DARK);
    gfx_text(c.x + c.w - 16 - gfx_text_width(when), c.y + CAL_FOOTER_Y + 4, when, ACCENT);
}

static void calendar_go_today(struct calendar *cal)
{
    struct rtc_time t;
    rtc_read(&t);
    cal->sel_day = t.day;
    cal->sel_month = cal->view_month = t.month;
    cal->sel_year = cal->view_year = t.year;
}

static void calendar_click(struct window *w, int x, int y, int dbl)
{
    (void)dbl;
    struct calendar *cal = w->state;
    struct rect area = centered(gui_client_size(w), CAL_W, CAL_H);
    x -= area.x;                                       /* wspolrzedne wzgledem kalendarza */
    y -= area.y;
    struct rect c = { 0, 0, CAL_W, CAL_H };
    struct rect p = cal_btn_prev(c), n = cal_btn_next(c), t = cal_btn_today(c);

    if (rect_contains(&p, x, y)) {
        add_months(&cal->view_year, &cal->view_month, -1);
    } else if (rect_contains(&n, x, y)) {
        add_months(&cal->view_year, &cal->view_month, 1);
    } else if (rect_contains(&t, x, y)) {
        calendar_go_today(cal);
    } else if (x >= CAL_GRID_X && x < CAL_GRID_X + 7 * CAL_CELL_W &&
               y >= CAL_GRID_Y && y < CAL_GRID_Y + 6 * CAL_CELL_H) {
        int i = (y - CAL_GRID_Y) / CAL_CELL_H * 7 + (x - CAL_GRID_X) / CAL_CELL_W;
        cell_date(cal, i, &cal->sel_year, &cal->sel_month, &cal->sel_day);
        cal->view_month = cal->sel_month;               /* klik w dzien sasiedniego miesiaca przelacza */
        cal->view_year = cal->sel_year;
    } else {
        return;
    }
    gui_redraw(w);
}

static void calendar_key(struct window *w, int key)
{
    struct calendar *cal = w->state;
    int step = 0;
    switch (key) {
    case KEY_LEFT:  step = -1; break;
    case KEY_RIGHT: step = 1; break;
    case KEY_UP:    step = -7; break;
    case KEY_DOWN:  step = 7; break;
    case KEY_PGUP:  add_months(&cal->view_year, &cal->view_month, -1); gui_redraw(w); return;
    case KEY_PGDN:  add_months(&cal->view_year, &cal->view_month, 1); gui_redraw(w); return;
    case KEY_HOME:  calendar_go_today(cal); gui_redraw(w); return;
    default: return;
    }
    add_days(&cal->sel_year, &cal->sel_month, &cal->sel_day, step);
    cal->view_month = cal->sel_month;
    cal->view_year = cal->sel_year;
    gui_redraw(w);
}

static const struct app calendar_app = {
    .draw = calendar_draw, .click = calendar_click, .key = calendar_key,
    .destroy = free_state, .live = 1, .min_w = CAL_W, .min_h = CAL_H,
};

void app_open_calendar(void)
{
    struct calendar *cal = kzalloc(sizeof(*cal));
    if (!cal)
        return;
    calendar_go_today(cal);
    if (!gui_open(&calendar_app, "Kalendarz", CAL_W, CAL_H, cal))
        kfree(cal);
}

/* ================= Terminal ================= */

#define TERM_COLS 90
#define TERM_ROWS 28
#define TERM_PAD  6
#define BLINK_TICKS 50

struct terminal {
    struct console *con;
    int thread;
    int blink_on;
    uint32_t blink_at;
};

/* Plotno programu stoi na srodku obszaru roboczego (gdy okno jest wieksze) */
static struct rect canvas_area(struct rect c, const struct console *con)
{
    return centered(c, con->canvas_w, con->canvas_h);
}

static void terminal_draw(struct window *w, struct rect c)
{
    struct terminal *t = w->state;
    struct console *con = t->con;
    gfx_fill(c.x, c.y, c.w, c.h, 0x000000);
    if (con->canvas) {                  /* program rysuje grafike zamiast tekstu */
        struct rect a = canvas_area(c, con);
        gfx_blit(a.x, a.y, con->canvas_w, con->canvas_h, con->canvas);
        return;
    }
    for (unsigned row = 0; row < con->rows; row++)
        for (unsigned col = 0; col < con->cols; col++) {
            uint16_t cell = con->cells[row * con->cols + col];
            uint8_t attr = (uint8_t)(cell >> 8);
            gfx_char(c.x + TERM_PAD + col * 8, c.y + TERM_PAD + row * 16, (unsigned char)cell,
                     vga_palette[attr & 0x0F], vga_palette[attr >> 4]);
        }
    if (!con->cursor_hidden && t->blink_on && gui_is_focused(w)) {
        uint8_t attr = (uint8_t)(con->cells[con->row * con->cols + con->col] >> 8);
        gfx_fill(c.x + TERM_PAD + con->col * 8, c.y + TERM_PAD + con->row * 16 + 13, 8, 2,
                 vga_palette[attr & 0x0F] ? vga_palette[attr & 0x0F] : 0xAAAAAA);
    }
}

static void terminal_tick(struct window *w)
{
    struct terminal *t = w->state;
    if (!thread_alive(t->thread)) {     /* program w oknie sie skonczyl - zamykamy okno */
        gui_close(w);
        return;
    }
    if (timer_ticks() - t->blink_at >= BLINK_TICKS) {
        t->blink_at = timer_ticks();
        t->blink_on = !t->blink_on;
        t->con->dirty = 1;
    }
    struct console *con = t->con;
    if (con->canvas_resize) {           /* program otworzyl albo zamknal plotno */
        con->canvas_resize = 0;
        if (con->canvas)
            gui_set_client_size(w, con->canvas_w, con->canvas_h);
        else
            gui_set_client_size(w, (int)con->cols * 8 + 2 * TERM_PAD, (int)con->rows * 16 + 2 * TERM_PAD);
    }
    if (con->canvas) {                  /* mysz dla programu: wspolrzedne na plotnie */
        int mx, my;
        mouse_position(&mx, &my);
        struct rect a = canvas_area(gui_client_screen(w), con);
        con->mouse_x = mx - a.x;
        con->mouse_y = my - a.y;
        con->mouse_buttons = gui_is_focused(w) && rect_contains(&a, mx, my) ? mouse_buttons() : 0;
    }
    if (con->dirty) {
        con->dirty = 0;
        gui_redraw(w);
    }
}

static void terminal_key(struct window *w, int key)
{
    struct terminal *t = w->state;
    t->blink_on = 1;                    /* przy pisaniu kursor jest widoczny */
    t->blink_at = timer_ticks();
    console_push_key(t->con, key);
}

static void terminal_click(struct window *w, int x, int y, int dbl)
{
    (void)dbl;
    struct terminal *t = w->state;
    if (x < TERM_PAD || y < TERM_PAD)
        return;
    t->con->click_x = (unsigned)(x - TERM_PAD) / 8;
    t->con->click_y = (unsigned)(y - TERM_PAD) / 16;
    console_push_key(t->con, KEY_MOUSE_LEFT);
}

/* Powiekszone okno = wiecej kolumn i wierszy tekstu */
static void terminal_resize(struct window *w)
{
    struct terminal *t = w->state;
    struct rect c = gui_client_size(w);
    unsigned cols = (unsigned)(c.w - 2 * TERM_PAD) / 8, rows = (unsigned)(c.h - 2 * TERM_PAD) / 16;
    if (console_resize(t->con, cols, rows))
        console_push_key(t->con, KEY_REFRESH);  /* np. edytor od razu przerysuje sie na nowo */
}

static void terminal_destroy(struct window *w)
{
    struct terminal *t = w->state;
    t->con->closed = 1;                 /* programy w konsoli dostana "wyjdz" */
    for (int i = 0; i < 1000 && thread_alive(t->thread); i++)
        thread_yield();
    if (!thread_alive(t->thread))       /* jesli sie nie skonczyl, konsola musi zostac */
        console_destroy(t->con);
    kfree(t);
}

static const struct app terminal_app = {
    .draw = terminal_draw, .click = terminal_click, .key = terminal_key,
    .destroy = terminal_destroy, .tick = terminal_tick, .resize = terminal_resize,
    .wants_escape = 1, .min_w = 40 * 8 + 2 * TERM_PAD, .min_h = 8 * 16 + 2 * TERM_PAD,
};

void app_open_terminal(const char *title, void (*fn)(void *), void *arg)
{
    struct terminal *t = kzalloc(sizeof(*t));
    if (!t)
        return;
    t->con = console_create(TERM_COLS, TERM_ROWS);
    if (!t->con) {
        kfree(t);
        return;
    }
    t->thread = -1;
    struct window *w = gui_open(&terminal_app, title, TERM_COLS * 8 + 2 * TERM_PAD,
                                TERM_ROWS * 16 + 2 * TERM_PAD, t);
    if (!w) {
        console_destroy(t->con);
        kfree(t);
        return;
    }
    t->thread = thread_create(fn, arg, t->con);
    if (t->thread < 0)
        gui_close(w);                   /* za duzo zadan albo brak pamieci */
}

/* ================= Programy .exe ================= */

/* Zadanie okna programu: uruchamia go, a po zakonczeniu pokazuje wynik
 * i czeka na klawisz (inaczej tekst programu zniknalby razem z oknem) */
static void run_program_window(void *arg)
{
    char *path = arg;
    const char *error;
    int code = process_run(path, &error);
    kfree(path);
    if (code == PROCESS_KILLED)
        return;                             /* okno zamkniete w trakcie */
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    if (error && code != PROCESS_CRASHED) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        kprintf("\nNie mozna uruchomic programu: %s\n", error);
    } else if (code != PROCESS_CRASHED) {
        kprintf("\n[Program zakonczyl dzialanie, kod %d]\n", code);
    }
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_write("Nacisnij dowolny klawisz, aby zamknac okno.");
    keyboard_getkey();
}

void app_open_program(const char *path)
{
    char *copy = kmalloc(strlen(path) + 1);
    if (!copy)
        return;
    strcpy(copy, path);
    const char *name = path;
    for (const char *s = path; *s; s++)
        if (*s == '/')
            name = s + 1;
    app_open_terminal(name, run_program_window, copy);
}

/* ================= Notatnik (podglad pliku) ================= */

struct notepad {
    struct fs_node *file;
    char path[FS_PATH_MAX];
    int scroll;                 /* pierwsza widoczna linia */
};

#define NOTE_TOOLBAR 34
#define NOTE_GUTTER  44

static struct rect note_edit_button(struct rect c)
{
    struct rect r = { c.x + 8, c.y + 6, 80, 22 };
    return r;
}

static int count_lines(const struct fs_node *f)
{
    int n = 1;
    for (uint32_t i = 0; i < f->size; i++)
        if (f->data[i] == '\n' && i + 1 < f->size)
            n++;
    return n;
}

static void notepad_draw(struct window *w, struct rect c)
{
    struct notepad *np = w->state;
    gfx_fill(c.x, c.y, c.w, NOTE_TOOLBAR, 0xE5E7EB);
    gfx_fill(c.x, c.y + NOTE_TOOLBAR - 1, c.w, 1, 0xD1D5DB);
    button(note_edit_button(c), "Edytuj");
    gfx_text(c.x + 100, c.y + 9, np->path, TEXT_MUTED);

    int top = c.y + NOTE_TOOLBAR;
    gfx_fill(c.x, top, c.w, c.h - NOTE_TOOLBAR, 0xFFFFFF);
    gfx_fill(c.x, top, NOTE_GUTTER, c.h - NOTE_TOOLBAR, 0xF3F4F6);

    const struct fs_node *f = np->file;
    int visible = (c.h - NOTE_TOOLBAR - 8) / LINE_H;
    int line = 0;
    uint32_t i = 0;
    char buf[256];
    while (i <= f->size && line < np->scroll + visible) {
        uint32_t start = i;
        while (i < f->size && f->data[i] != '\n')
            i++;
        if (start == f->size && line > 0)
            break;                          /* '\n' na koncu pliku nie tworzy pustej linii */
        if (line >= np->scroll) {
            int y = top + 4 + (line - np->scroll) * LINE_H;
            char num[8];
            ksnprintf(num, sizeof(num), "%3u", line + 1);
            gfx_text(c.x + 6, y, num, 0x9CA3AF);
            uint32_t n = i - start < sizeof(buf) - 1 ? i - start : sizeof(buf) - 1;
            memcpy(buf, f->data + start, n);
            buf[n] = '\0';
            gfx_text(c.x + NOTE_GUTTER + 8, y, buf, TEXT_DARK);
        }
        line++;
        i++;                                /* pomijamy '\n' */
    }
}

/* Edytor w oknie terminala; sciezke dostaje w kopii z kmalloc i ja zwalnia */
static void run_editor(void *arg)
{
    char *path = arg;
    editor_run(fs_root(), path);
    kfree(path);
}

static void notepad_click(struct window *w, int x, int y, int dbl)
{
    (void)dbl;
    struct notepad *np = w->state;
    struct rect c = { 0, 0, 0, 0 };
    struct rect b = note_edit_button(c);
    if (!rect_contains(&b, x, y))
        return;
    char *path = kmalloc(strlen(np->path) + 1);
    if (!path)
        return;
    strcpy(path, np->path);
    char title[48];
    ksnprintf(title, sizeof(title), "Edytor - %s", np->file->name);
    app_open_terminal(title, run_editor, path);
}

static void notepad_key(struct window *w, int key)
{
    struct notepad *np = w->state;
    int lines = count_lines(np->file);
    int page = (w->r.h - GUI_TITLE_H - NOTE_TOOLBAR) / LINE_H - 1;
    switch (key) {
    case KEY_UP:   np->scroll--; break;
    case KEY_DOWN: np->scroll++; break;
    case KEY_PGUP: np->scroll -= page; break;
    case KEY_PGDN: np->scroll += page; break;
    case KEY_HOME: np->scroll = 0; break;
    case KEY_END:  np->scroll = lines - page; break;
    default: return;
    }
    if (np->scroll > lines - 1)
        np->scroll = lines - 1;
    if (np->scroll < 0)
        np->scroll = 0;
    gui_redraw(w);
}

/* Plik usuniety w terminalu - zamykamy podglad, zanim siegniemy do zwolnionej pamieci */
static void notepad_tick(struct window *w)
{
    struct notepad *np = w->state;
    if (!fs_node_valid(np->file))
        gui_close(w);
}

/* live: po zapisie w edytorze tresc odswiezy sie sama */
static const struct app notepad_app = {
    .draw = notepad_draw, .click = notepad_click, .key = notepad_key,
    .destroy = free_state, .tick = notepad_tick, .live = 1, .min_w = 320, .min_h = 150,
};

static void open_notepad(struct fs_node *file)
{
    struct notepad *np = kzalloc(sizeof(*np));
    if (!np)
        return;
    np->file = file;
    fs_path(file, np->path, sizeof(np->path));
    char title[48];
    ksnprintf(title, sizeof(title), "Notatnik - %s", file->name);
    if (!gui_open(&notepad_app, title, 620, 400, np))
        kfree(np);
}

/* ================= Pliki ================= */

struct files {
    struct fs_node *dir;
    int selected;               /* indeks na liscie (".." liczy sie jako 0, jesli jest) */
    int scroll;
};

#define FILES_TOOLBAR 30
#define FILES_ROW     22

static int has_parent(const struct files *fl)
{
    return fl->dir != fs_root();
}

static int entry_count(const struct files *fl)
{
    int n = has_parent(fl);
    for (struct fs_node *c = fl->dir->children; c; c = c->next)
        n++;
    return n;
}

/* NULL oznacza ".." */
static struct fs_node *entry_at(const struct files *fl, int i)
{
    if (has_parent(fl) && i-- == 0)
        return 0;
    struct fs_node *c = fl->dir->children;
    while (c && i-- > 0)
        c = c->next;
    return c;
}

static int is_program(const struct fs_node *e)
{
    return e && !e->is_dir && e->size >= 4 && memcmp(e->data, "\x7F" "ELF", 4) == 0;
}

static void small_icon(int x, int y, int is_dir)
{
    if (is_dir == 2) {                              /* program: okienko z paskiem */
        gfx_fill(x, y + 1, 16, 14, 0xFFFFFF);
        gfx_frame(x, y + 1, 16, 14, 0x4B5563);
        gfx_fill(x + 1, y + 2, 14, 4, gui_accent_color());
        gfx_fill(x + 3, y + 8, 6, 2, 0x4ADE80);
    } else if (is_dir) {
        gfx_fill(x, y + 2, 7, 3, 0xD97706);
        gfx_fill(x, y + 4, 16, 11, 0xFBBF24);
    } else {
        gfx_fill(x + 2, y, 12, 16, 0xFFFFFF);
        gfx_frame(x + 2, y, 12, 16, 0x9CA3AF);
        for (int k = 0; k < 3; k++)
            gfx_fill(x + 4, y + 4 + k * 4, 8, 1, 0x9CA3AF);
    }
}

static int files_visible(struct window *w)
{
    return (w->r.h - GUI_TITLE_H - FILES_TOOLBAR - 6) / FILES_ROW;
}

static void files_draw(struct window *w, struct rect c)
{
    struct files *fl = w->state;
    char path[FS_PATH_MAX], line[64];
    fs_path(fl->dir, path, sizeof(path));

    gfx_fill(c.x, c.y, c.w, FILES_TOOLBAR, 0xE5E7EB);
    gfx_fill(c.x, c.y + FILES_TOOLBAR - 1, c.w, 1, 0xD1D5DB);
    small_icon(c.x + 10, c.y + 7, 1);
    gfx_text(c.x + 34, c.y + 7, path, TEXT_DARK);
    gfx_fill(c.x, c.y + FILES_TOOLBAR, c.w, c.h - FILES_TOOLBAR, 0xFFFFFF);

    int n = entry_count(fl), visible = files_visible(w);
    for (int i = fl->scroll; i < n && i < fl->scroll + visible; i++) {
        struct fs_node *e = entry_at(fl, i);
        int y = c.y + FILES_TOOLBAR + 4 + (i - fl->scroll) * FILES_ROW;
        int sel = i == fl->selected;
        if (sel)
            gfx_fill(c.x + 2, y - 2, c.w - 4, FILES_ROW, ACCENT);
        uint32_t fg = sel ? 0xFFFFFF : TEXT_DARK;
        small_icon(c.x + 10, y, is_program(e) ? 2 : !e || e->is_dir);
        gfx_text(c.x + 34, y, e ? e->name : "..", fg);
        if (is_program(e))
            ksnprintf(line, sizeof(line), "program, %u B", e->size);
        else if (e && !e->is_dir)
            ksnprintf(line, sizeof(line), "%u B", e->size);
        else
            ksnprintf(line, sizeof(line), "folder");
        gfx_text(c.x + c.w - 16 - gfx_text_width(line), y, line, sel ? 0xDBEAFE : TEXT_MUTED);
    }
    if (n == 0)
        gfx_text(c.x + 34, c.y + FILES_TOOLBAR + 8, "(pusty folder)", TEXT_MUTED);
}

static void files_update_title(struct window *w)
{
    struct files *fl = w->state;
    char path[FS_PATH_MAX], title[48];
    fs_path(fl->dir, path, sizeof(path));
    ksnprintf(title, sizeof(title), "Pliki - %s", path);
    gui_set_title(w, title);
}

static void files_open(struct window *w, int i)
{
    struct files *fl = w->state;
    if (i < 0 || i >= entry_count(fl))
        return;
    struct fs_node *e = entry_at(fl, i);
    if (!e || e->is_dir) {
        fl->dir = e ? e : fl->dir->parent;
        fl->selected = 0;
        fl->scroll = 0;
        files_update_title(w);
        gui_redraw(w);
    } else if (is_program(e)) {
        char path[FS_PATH_MAX];
        fs_path(e, path, sizeof(path));
        app_open_program(path);
    } else {
        open_notepad(e);
    }
}

static void files_keep_visible(struct window *w)
{
    struct files *fl = w->state;
    int visible = files_visible(w);
    if (fl->selected < fl->scroll)
        fl->scroll = fl->selected;
    if (fl->selected >= fl->scroll + visible)
        fl->scroll = fl->selected - visible + 1;
}

static void files_click(struct window *w, int x, int y, int dbl)
{
    (void)x;
    struct files *fl = w->state;
    if (y < FILES_TOOLBAR + 2)
        return;
    int i = fl->scroll + (y - FILES_TOOLBAR - 2) / FILES_ROW;
    if (i >= entry_count(fl))
        return;
    fl->selected = i;
    gui_redraw(w);
    if (dbl)
        files_open(w, i);
}

static void files_key(struct window *w, int key)
{
    struct files *fl = w->state;
    int n = entry_count(fl);
    switch (key) {
    case KEY_UP:   if (fl->selected > 0) fl->selected--; break;
    case KEY_DOWN: if (fl->selected < n - 1) fl->selected++; break;
    case '\n':     files_open(w, fl->selected); return;
    case '\b':
        if (has_parent(fl))
            files_open(w, 0);
        return;
    default: return;
    }
    files_keep_visible(w);
    gui_redraw(w);
}

/* Folder usuniety w terminalu - wracamy do katalogu glownego */
static void files_tick(struct window *w)
{
    struct files *fl = w->state;
    if (!fs_node_valid(fl->dir)) {
        fl->dir = fs_root();
        fl->selected = fl->scroll = 0;
        files_update_title(w);
        gui_redraw(w);
    }
}

/* live: zmiany zrobione w terminalu (np. nowy plik) pojawia sie same */
static const struct app files_app = {
    .draw = files_draw, .click = files_click, .key = files_key,
    .destroy = free_state, .tick = files_tick, .live = 1, .min_w = 300, .min_h = 150,
};

void app_open_files(void)
{
    struct files *fl = kzalloc(sizeof(*fl));
    if (!fl)
        return;
    fl->dir = fs_root();
    if (!gui_open(&files_app, "Pliki - /", 460, 380, fl))
        kfree(fl);
}

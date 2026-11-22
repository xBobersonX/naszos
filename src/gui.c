/* Pulpit: tlo z ikonami, okna, pasek zadan z menu Start i zegarem.
 *
 * Rysowanie: kazda zmiana zglasza prostokat do odswiezenia (invalidate).
 * Raz na obrot petli skladamy tylko ten fragment: tlo -> ikony -> okna od
 * najnizszego do najwyzszego -> pasek zadan -> menu, a potem kopiujemy go
 * na ekran. Okna sa trzymane w tablicy order[] - ostatnie jest na wierzchu
 * i ma "fokus" (dostaje klawisze). Zminimalizowane okna nie sa rysowane,
 * widac je tylko na pasku zadan. */

#include "gui.h"
#include "gfx.h"
#include "vga.h"
#include "mouse.h"
#include "keyboard.h"
#include "timer.h"
#include "rtc.h"
#include "string.h"
#include "power.h"
#include "shell.h"
#include "thread.h"
#include "kernel.h"

#define TASKBAR_H   40
#define MAX_WINDOWS 8
#define DOUBLE_CLICK_TICKS 45    /* 450 ms */

#define COLOR_TASKBAR_TOP    0x1F2937
#define COLOR_TASKBAR_BOTTOM 0x111827
#define COLOR_TEXT_LIGHT     0xF3F4F6
#define COLOR_TITLE_IDLE1    0x6B7280
#define COLOR_TITLE_IDLE2    0x4B5563
#define COLOR_WINDOW_BG      0xF3F4F6
#define COLOR_BORDER         0x111827
#define COLOR_CLOSE          0xDC2626
#define COLOR_MINIMIZE       0x475569

static int W, H;
static struct window windows[MAX_WINDOWS];
static struct window *order[MAX_WINDOWS];
static int count;

static struct rect dirty;
static int quit;
static int menu_open, menu_hover = -1;
static int selected_icon = -1;

static struct window *dragging;
static int drag_dx, drag_dy;

/* Zmiana rozmiaru: ktore krawedzie ciagniemy, i stan z chwili chwycenia */
enum { EDGE_LEFT = 1, EDGE_RIGHT = 2, EDGE_TOP = 4, EDGE_BOTTOM = 8 };
#define GRIP 6                   /* szerokosc pasa przy krawedzi, za ktory mozna chwycic */
static struct window *resizing;
static int resize_edges, resize_mx, resize_my;
static struct rect resize_start;

/* Powiekszone okno "odkleja sie" dopiero, gdy mysz ruszy sie z wcisnietym przyciskiem */
static struct window *unsnap;
static int unsnap_x, unsnap_y;

static uint32_t last_click_tick;
static int last_click_x, last_click_y;

/* ---------- pomocnicze ---------- */

static void invalidate(struct rect r)
{
    dirty = rect_union(dirty, r);
}

static void invalidate_all(void)
{
    struct rect r = { 0, 0, W, H };
    invalidate(r);
}

static void invalidate_window(struct window *w)
{
    struct rect r = { w->r.x, w->r.y, w->r.w + 6, w->r.h + 6 };   /* z cieniem */
    invalidate(r);
}

static void invalidate_taskbar(void)
{
    struct rect bar = { 0, H - TASKBAR_H, W, TASKBAR_H };
    invalidate(bar);
}

static struct rect intersect(struct rect a, struct rect b)
{
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    struct rect r = { x0, y0, x1 > x0 ? x1 - x0 : 0, y1 > y0 ? y1 - y0 : 0 };
    return r;
}

static void text_centered(struct rect r, const char *s, uint32_t color)
{
    gfx_text(r.x + (r.w - gfx_text_width(s)) / 2, r.y + (r.h - 16) / 2, s, color);
}

static void text_shadow(int x, int y, const char *s, uint32_t color)
{
    gfx_text(x + 1, y + 1, s, 0x000000);
    gfx_text(x, y, s, color);
}

static struct rect client_rect(const struct window *w)
{
    struct rect r = { w->r.x + 1, w->r.y + GUI_TITLE_H, w->r.w - 2, w->r.h - GUI_TITLE_H - 1 };
    return r;
}

static struct rect close_rect(const struct window *w)
{
    struct rect r = { w->r.x + w->r.w - 24, w->r.y + 4, 18, 18 };
    return r;
}

static struct rect maximize_rect(const struct window *w)
{
    struct rect r = { w->r.x + w->r.w - 46, w->r.y + 4, 18, 18 };
    return r;
}

static struct rect minimize_rect(const struct window *w)
{
    struct rect r = { w->r.x + w->r.w - 68, w->r.y + 4, 18, 18 };
    return r;
}

struct rect gui_client_screen(const struct window *w)
{
    return client_rect(w);
}

struct rect gui_client_size(const struct window *w)
{
    struct rect c = client_rect(w);
    c.x = c.y = 0;
    return c;
}

/* Okno z fokusem = najwyzsze niezminimalizowane */
static struct window *focused(void)
{
    for (int i = count - 1; i >= 0; i--)
        if (!order[i]->minimized)
            return order[i];
    return 0;
}

int gui_is_focused(const struct window *w)
{
    return focused() == w;
}

/* ---------- okna ---------- */

struct window *gui_open(const struct app *app, const char *title, int w, int h, void *state)
{
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (!windows[i].used) {
            slot = i;
            break;
        }
    if (slot < 0)
        return 0;

    struct window *win = &windows[slot];
    memset(win, 0, sizeof(*win));
    win->used = 1;
    win->app = app;
    win->state = state;
    win->r.w = w + 2;
    win->r.h = h + GUI_TITLE_H + 1;
    win->r.x = 150 + count * 36;           /* kolejne okna "kaskada" */
    win->r.y = 40 + count * 36;
    if (win->r.x + win->r.w > W)
        win->r.x = W - win->r.w > 0 ? W - win->r.w : 0;
    if (win->r.y + win->r.h > H - TASKBAR_H)
        win->r.y = H - TASKBAR_H - win->r.h > 0 ? H - TASKBAR_H - win->r.h : 0;
    gui_set_title(win, title);

    struct window *prev = focused();
    order[count++] = win;
    if (prev)
        invalidate_window(prev);           /* traci fokus - inny kolor paska */
    invalidate_window(win);
    return win;
}

void gui_close(struct window *w)
{
    if (!w || !w->used)
        return;
    if (w->app->destroy)
        w->app->destroy(w);
    int i = 0;
    while (i < count && order[i] != w)
        i++;
    for (; i < count - 1; i++)
        order[i] = order[i + 1];
    count--;
    if (dragging == w)
        dragging = 0;
    if (resizing == w)
        resizing = 0;
    if (unsnap == w)
        unsnap = 0;
    if (!w->minimized)
        invalidate_window(w);
    w->used = 0;
    if (focused())
        invalidate_window(focused());
    invalidate_taskbar();
}

void gui_redraw(struct window *w)
{
    if (!w->minimized)
        invalidate(client_rect(w));
}

void gui_set_title(struct window *w, const char *title)
{
    size_t n = 0;
    while (title[n] && n < sizeof(w->title) - 1) {
        w->title[n] = title[n];
        n++;
    }
    w->title[n] = '\0';
    if (!w->minimized)
        invalidate_window(w);
    invalidate_taskbar();
}

static void move_in_order(struct window *w, int to_top)
{
    int i = 0;
    while (order[i] != w)
        i++;
    if (to_top) {
        for (; i < count - 1; i++)
            order[i] = order[i + 1];
        order[count - 1] = w;
    } else {
        for (; i > 0; i--)
            order[i] = order[i - 1];
        order[0] = w;
    }
}

static void focus(struct window *w)
{
    struct window *prev = focused();
    if (prev == w && !w->minimized)
        return;
    w->minimized = 0;
    move_in_order(w, 1);
    invalidate_window(w);
    if (prev)
        invalidate_window(prev);
    invalidate_taskbar();
}

static void minimize(struct window *w)
{
    if (w->minimized)
        return;
    invalidate_window(w);
    w->minimized = 1;
    move_in_order(w, 0);                   /* na spod - fokus dostaje nastepne okno */
    if (dragging == w)
        dragging = 0;
    if (focused())
        invalidate_window(focused());
    invalidate_taskbar();
}

/* Powiekszenie na caly pulpit (nad paskiem zadan) albo powrot do poprzedniego miejsca */
static void toggle_maximize(struct window *w)
{
    invalidate_window(w);
    if (w->maximized) {
        w->r = w->normal;
        w->maximized = 0;
    } else {
        w->normal = w->r;
        w->r.x = 0;
        w->r.y = 0;
        w->r.w = W;
        w->r.h = H - TASKBAR_H;
        w->maximized = 1;
    }
    if (dragging == w)
        dragging = 0;
    if (w->app->resize)
        w->app->resize(w);
    invalidate_window(w);
}

void gui_maximize(struct window *w)
{
    if (!w->maximized)
        toggle_maximize(w);
}

/* Program chce inny rozmiar okna (np. plotno graficzne) */
void gui_set_client_size(struct window *w, int cw, int ch)
{
    if (w->maximized)
        return;                          /* powiekszone zostaje powiekszone - tresc stanie na srodku */
    invalidate_window(w);
    w->r.w = cw + 2;
    w->r.h = ch + GUI_TITLE_H + 1;
    if (w->r.x + w->r.w > W)
        w->r.x = W - w->r.w > 0 ? W - w->r.w : 0;
    if (w->r.y + w->r.h > H - TASKBAR_H)
        w->r.y = H - TASKBAR_H - w->r.h > 0 ? H - TASKBAR_H - w->r.h : 0;
    invalidate_window(w);
}

static void draw_window(struct window *w, int active)
{
    struct rect r = w->r;
    if (!w->maximized)
        gfx_darken(r.x + 6, r.y + 6, r.w, r.h);            /* cien */
    gfx_frame(r.x, r.y, r.w, r.h, COLOR_BORDER);
    gfx_gradient(r.x + 1, r.y + 1, r.w - 2, GUI_TITLE_H - 1,
                 active ? gui_accent_color() : COLOR_TITLE_IDLE1,
                 active ? gui_accent_dark() : COLOR_TITLE_IDLE2);
    text_shadow(r.x + 10, r.y + 5, w->title, 0xFFFFFF);

    struct rect m = minimize_rect(w);                       /* pomniejsz: kreska */
    gfx_fill(m.x, m.y, m.w, m.h, COLOR_MINIMIZE);
    gfx_frame(m.x, m.y, m.w, m.h, 0x94A3B8);
    gfx_fill(m.x + 4, m.y + 12, 10, 2, 0xFFFFFF);

    struct rect x = maximize_rect(w);                       /* powieksz: kwadrat, przywroc: dwa */
    gfx_fill(x.x, x.y, x.w, x.h, COLOR_MINIMIZE);
    gfx_frame(x.x, x.y, x.w, x.h, 0x94A3B8);
    if (w->maximized) {
        gfx_frame(x.x + 6, x.y + 3, 9, 8, 0xFFFFFF);
        gfx_fill(x.x + 6, x.y + 3, 9, 2, 0xFFFFFF);
        gfx_fill(x.x + 3, x.y + 7, 9, 8, COLOR_MINIMIZE);   /* przedni kwadrat zaslania tylny */
        gfx_frame(x.x + 3, x.y + 7, 9, 8, 0xFFFFFF);
        gfx_fill(x.x + 3, x.y + 7, 9, 2, 0xFFFFFF);
    } else {
        gfx_frame(x.x + 4, x.y + 4, 10, 10, 0xFFFFFF);
        gfx_fill(x.x + 4, x.y + 4, 10, 2, 0xFFFFFF);
    }

    struct rect c = close_rect(w);                          /* zamknij: krzyzyk */
    gfx_fill(c.x, c.y, c.w, c.h, COLOR_CLOSE);
    gfx_frame(c.x, c.y, c.w, c.h, 0xFCA5A5);
    for (int i = 0; i < 10; i++) {
        gfx_fill(c.x + 4 + i, c.y + 4 + i, 2, 2, 0xFFFFFF);
        gfx_fill(c.x + 13 - i, c.y + 4 + i, 2, 2, 0xFFFFFF);
    }

    struct rect client = client_rect(w);
    gfx_fill(client.x, client.y, client.w, client.h, COLOR_WINDOW_BG);
    if (w->app->draw) {
        struct rect saved = gfx_get_clip();
        struct rect inner = intersect(saved, client);
        if (inner.w > 0 && inner.h > 0) {
            gfx_set_clip(&inner);                           /* program nie wyjdzie poza okno */
            w->app->draw(w, client);
            gfx_set_clip(&saved);
        }
    }
}

/* ---------- ikony pulpitu ---------- */

enum { ICON_TERMINAL, ICON_BROWSER, ICON_FILES, ICON_SNAKE, ICON_PIANO, ICON_CALENDAR, ICON_SETTINGS, ICON_ABOUT, ICON_COUNT };
static const char *const icon_names[ICON_COUNT] = {
    "Terminal", "Przegladarka", "Pliki", "Snake", "Pianino", "Kalendarz", "Ustawienia", "O systemie",
};

static struct rect icon_rect(int i)
{
    int rows = (H - TASKBAR_H - 16) / 100;          /* nie mieszcza sie - druga kolumna */
    if (rows < 1)
        rows = 1;
    struct rect r = { 16 + (i / rows) * 104, 16 + (i % rows) * 100, 96, 92 };
    return r;
}

static void draw_pictogram(int i, int x, int y)
{
    switch (i) {
    case ICON_TERMINAL:
        gfx_fill(x, y, 48, 42, 0x111111);
        gfx_frame(x, y, 48, 42, 0x9CA3AF);
        gfx_fill(x + 1, y + 1, 46, 7, 0x4B5563);
        gfx_text(x + 6, y + 16, ">_", 0x4ADE80);
        break;
    case ICON_BROWSER: {                                   /* kula ziemska z poludnikami */
        static const int half[22] = { 5, 9, 11, 13, 14, 16, 17, 18, 19, 19, 20, 20, 20, 20, 19, 19, 18, 17, 16, 14, 13, 11 };
        for (int k = 0; k < 44; k++) {
            int hw = half[k < 22 ? k : 43 - k];
            gfx_fill(x + 24 - hw, y + k, 2 * hw, 1, 0x2563EB);
        }
        for (int k = 4; k < 40; k++) {                     /* poludnik i rownik */
            gfx_fill(x + 23, y + k, 2, 1, 0xBFDBFE);
        }
        gfx_fill(x + 5, y + 21, 38, 2, 0xBFDBFE);
        gfx_fill(x + 9, y + 11, 30, 1, 0x93C5FD);
        gfx_fill(x + 9, y + 32, 30, 1, 0x93C5FD);
        gfx_fill(x + 12, y + 14, 8, 6, 0x22C55E);          /* "lady" */
        gfx_fill(x + 27, y + 25, 9, 7, 0x22C55E);
        break;
    }
    case ICON_FILES:
        gfx_fill(x + 2, y + 6, 20, 8, 0xD97706);           /* zakladka teczki */
        gfx_fill(x, y + 12, 48, 32, 0xFBBF24);
        gfx_frame(x, y + 12, 48, 32, 0xB45309);
        break;
    case ICON_SNAKE: {
        static const int path[][2] = { {0,4}, {1,4}, {2,4}, {2,3}, {2,2}, {3,2}, {4,2}, {4,1} };
        gfx_fill(x, y, 48, 44, 0x14532D);
        for (unsigned k = 0; k < sizeof(path) / sizeof(path[0]); k++)
            gfx_fill(x + 3 + path[k][0] * 8, y + 2 + path[k][1] * 8, 7, 7, k == 7 ? 0xBEF264 : 0x4ADE80);
        gfx_fill(x + 36, y + 30, 7, 7, 0xEF4444);         /* jablko */
        break;
    }
    case ICON_PIANO:                                       /* klawisze fortepianu */
        gfx_fill(x, y + 6, 48, 34, 0xFFFFFF);
        gfx_frame(x, y + 6, 48, 34, 0x374151);
        for (int k = 1; k < 6; k++)
            gfx_fill(x + k * 8, y + 6, 1, 34, 0x9CA3AF);
        for (int k = 0; k < 5; k++)
            if (k != 2)
                gfx_fill(x + 5 + k * 8, y + 6, 6, 20, 0x111111);
        break;
    case ICON_CALENDAR: {
        struct rtc_time t;
        char day[4];
        rtc_read(&t);
        ksnprintf(day, sizeof(day), "%u", t.day);
        gfx_fill(x + 2, y, 44, 44, 0xFFFFFF);
        gfx_fill(x + 2, y, 44, 12, 0xDC2626);
        gfx_frame(x + 2, y, 44, 44, 0x6B7280);
        gfx_text_scaled(x + 24 - gfx_text_width(day), y + 14, day, 0x111827, 2);
        break;
    }
    case ICON_SETTINGS: {                                  /* suwaki */
        gfx_fill(x + 2, y, 44, 44, 0x374151);
        gfx_frame(x + 2, y, 44, 44, 0x9CA3AF);
        static const int knob[3] = { 10, 28, 18 };
        for (int k = 0; k < 3; k++) {
            gfx_fill(x + 8, y + 10 + k * 12, 32, 2, 0xD1D5DB);
            gfx_fill(x + 6 + knob[k], y + 7 + k * 12, 8, 8, gui_accent_color());
            gfx_frame(x + 6 + knob[k], y + 7 + k * 12, 8, 8, 0xFFFFFF);
        }
        break;
    }
    case ICON_ABOUT:
        gfx_fill(x + 2, y, 44, 44, 0x2563EB);
        gfx_frame(x + 2, y, 44, 44, 0x93C5FD);
        gfx_text_scaled(x + 16, y + 6, "i", 0xFFFFFF, 2);
        break;
    }
}

static void draw_icons(void)
{
    for (int i = 0; i < ICON_COUNT; i++) {
        struct rect r = icon_rect(i);
        if (i == selected_icon)
            gfx_lighten(r.x, r.y, r.w, r.h);
        draw_pictogram(i, r.x + (r.w - 48) / 2, r.y + 8);
        text_shadow(r.x + (r.w - gfx_text_width(icon_names[i])) / 2, r.y + 64, icon_names[i], 0xFFFFFF);
    }
}

/* ---------- pasek zadan i menu Start ---------- */

static struct rect start_rect(void)
{
    struct rect r = { 4, H - TASKBAR_H + 4, 96, TASKBAR_H - 8 };
    return r;
}

/* n-ty przycisk okna na pasku (kolejnosc jak w tablicy windows[]) */
static struct rect task_button(int n)
{
    struct rect r = { 108 + n * 172, H - TASKBAR_H + 4, 168, TASKBAR_H - 8 };
    return r;
}

static void draw_taskbar(void)
{
    gfx_gradient(0, H - TASKBAR_H, W, TASKBAR_H, COLOR_TASKBAR_TOP, COLOR_TASKBAR_BOTTOM);
    gfx_fill(0, H - TASKBAR_H, W, 1, 0x374151);

    struct rect s = start_rect();
    uint32_t start_top = gfx_mix(gui_accent_color(), 0xFFFFFF, menu_open ? 2 : 1, 5);
    gfx_gradient(s.x, s.y, s.w, s.h, start_top, gui_accent_dark());
    gfx_frame(s.x, s.y, s.w, s.h, gfx_mix(gui_accent_dark(), 0x000000, 1, 3));
    gfx_fill(s.x + 10, s.y + 9, 6, 6, 0xEF4444);            /* logo: 4 kolorowe kwadraty */
    gfx_fill(s.x + 17, s.y + 9, 6, 6, 0x22C55E);
    gfx_fill(s.x + 10, s.y + 16, 6, 6, 0x3B82F6);
    gfx_fill(s.x + 17, s.y + 16, 6, 6, 0xFACC15);
    text_shadow(s.x + 32, s.y + (s.h - 16) / 2, "Start", 0xFFFFFF);

    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!windows[i].used)
            continue;
        struct rect b = task_button(n++);
        struct window *w = &windows[i];
        int active = focused() == w;
        gfx_fill(b.x, b.y, b.w, b.h, active ? 0x4B5563 : w->minimized ? 0x1F2937 : 0x374151);
        gfx_frame(b.x, b.y, b.w, b.h, w->minimized ? 0x374151 : 0x4B5563);
        if (active)
            gfx_fill(b.x, b.y + b.h - 3, b.w, 3, gfx_mix(gui_accent_color(), 0xFFFFFF, 1, 3));
        char label[20];
        ksnprintf(label, sizeof(label), "%s", w->title);
        if (strlen(w->title) > 19)
            label[16] = label[17] = label[18] = '.';
        gfx_text(b.x + 8, b.y + (b.h - 16) / 2, label, w->minimized ? 0x9CA3AF : COLOR_TEXT_LIGHT);
    }

    struct rtc_time t;
    rtc_read(&t);
    char time[16], date[16];
    if (gui_get_settings()->seconds)
        ksnprintf(time, sizeof(time), "%02u:%02u:%02u", t.hour, t.minute, t.second);
    else
        ksnprintf(time, sizeof(time), "%02u:%02u", t.hour, t.minute);
    ksnprintf(date, sizeof(date), "%s %02u.%02u.%u", rtc_day_short[t.weekday], t.day, t.month, t.year);
    struct rect clock = { W - 132, H - TASKBAR_H + 3, 128, 17 };
    text_centered(clock, time, COLOR_TEXT_LIGHT);
    clock.y += 17;
    text_centered(clock, date, 0x9CA3AF);
}

enum { MENU_TERMINAL, MENU_BROWSER, MENU_FILES, MENU_CALENDAR, MENU_SNAKE, MENU_PIANO, MENU_SETTINGS, MENU_ABOUT,
       MENU_SEPARATOR, MENU_TEXT_MODE, MENU_REBOOT, MENU_SHUTDOWN, MENU_COUNT };
static const char *const menu_names[MENU_COUNT] = {
    "Terminal", "Przegladarka", "Pliki", "Kalendarz", "Snake", "Pianino", "Ustawienia", "O systemie", "",
    "Tryb tekstowy", "Uruchom ponownie", "Wylacz komputer",
};
#define MENU_W        240
#define MENU_HEADER_H 40
#define MENU_ITEM_H   30

static struct rect menu_rect(void)
{
    int h = MENU_HEADER_H + MENU_COUNT * MENU_ITEM_H;
    struct rect r = { 4, H - TASKBAR_H - h - 4, MENU_W, h };
    return r;
}

static int menu_item_at(int x, int y)
{
    struct rect m = menu_rect();
    if (!rect_contains(&m, x, y) || y < m.y + MENU_HEADER_H)
        return -1;
    int i = (y - m.y - MENU_HEADER_H) / MENU_ITEM_H;
    return i == MENU_SEPARATOR ? -1 : i;
}

static void draw_menu(void)
{
    struct rect m = menu_rect();
    gfx_darken(m.x + 6, m.y + 6, m.w, m.h);
    gfx_fill(m.x, m.y, m.w, m.h, 0x1F2937);
    gfx_frame(m.x, m.y, m.w, m.h, 0x4B5563);
    gfx_gradient(m.x + 1, m.y + 1, m.w - 2, MENU_HEADER_H - 2, gui_accent_color(), gui_accent_dark());
    text_shadow(m.x + 14, m.y + 12, OS_NAME " v" OS_VERSION, 0xFFFFFF);

    for (int i = 0; i < MENU_COUNT; i++) {
        int y = m.y + MENU_HEADER_H + i * MENU_ITEM_H;
        if (i == MENU_SEPARATOR) {
            gfx_fill(m.x + 10, y + MENU_ITEM_H / 2, m.w - 20, 1, 0x4B5563);
            continue;
        }
        if (i == menu_hover)
            gfx_fill(m.x + 2, y, m.w - 4, MENU_ITEM_H, gui_accent_color());
        gfx_text(m.x + 20, y + (MENU_ITEM_H - 16) / 2, menu_names[i], COLOR_TEXT_LIGHT);
    }
}

static void set_menu(int open)
{
    menu_open = open;
    menu_hover = -1;
    struct rect shadow = menu_rect();
    shadow.w += 6;
    shadow.h += 6;
    invalidate(shadow);
    invalidate(start_rect());
}

/* ---------- akcje ---------- */

static void launch(int icon)
{
    switch (icon) {
    case ICON_TERMINAL: app_open_terminal("Terminal", shell_terminal, 0); break;
    case ICON_BROWSER:  app_open_browser(); break;
    case ICON_FILES:    app_open_files(); break;
    case ICON_SNAKE:    app_open_snake(); break;
    case ICON_PIANO:    app_open_piano(); break;
    case ICON_CALENDAR: app_open_calendar(); break;
    case ICON_SETTINGS: app_open_settings(); break;
    case ICON_ABOUT:    app_open_about(); break;
    }
}

static void menu_action(int item)
{
    switch (item) {
    case MENU_TERMINAL:  launch(ICON_TERMINAL); break;
    case MENU_BROWSER:   launch(ICON_BROWSER); break;
    case MENU_FILES:     launch(ICON_FILES); break;
    case MENU_CALENDAR:  launch(ICON_CALENDAR); break;
    case MENU_SNAKE:     launch(ICON_SNAKE); break;
    case MENU_PIANO:     launch(ICON_PIANO); break;
    case MENU_SETTINGS:  launch(ICON_SETTINGS); break;
    case MENU_ABOUT:     launch(ICON_ABOUT); break;
    case MENU_TEXT_MODE: quit = 1; break;
    case MENU_REBOOT:    gfx_end(); system_reboot();
    case MENU_SHUTDOWN:  gfx_end(); system_shutdown();
    }
}

/* ---------- obsluga myszy i klawiatury ---------- */

static void select_icon(int i)
{
    if (i == selected_icon)
        return;
    if (selected_icon >= 0)
        invalidate(icon_rect(selected_icon));
    if (i >= 0)
        invalidate(icon_rect(i));
    selected_icon = i;
}

static int edges_at(const struct window *w, int x, int y)
{
    if (w->maximized)
        return 0;
    int e = 0;
    if (x < w->r.x + GRIP)
        e |= EDGE_LEFT;
    if (x >= w->r.x + w->r.w - GRIP)
        e |= EDGE_RIGHT;
    if (y < w->r.y + 3)                     /* u gory waski pas - nizej jest pasek tytulu */
        e |= EDGE_TOP;
    if (y >= w->r.y + w->r.h - GRIP)
        e |= EDGE_BOTTOM;
    return e;
}

static void do_resize(int x, int y)
{
    const struct app *a = resizing->app;
    int min_w = (a->min_w ? a->min_w : 220) + 2;
    int min_h = (a->min_h ? a->min_h : 100) + GUI_TITLE_H + 1;
    if (min_w < 120)
        min_w = 120;
    int dx = x - resize_mx, dy = y - resize_my;
    struct rect s0 = resize_start, r = resize_start;

    if (resize_edges & EDGE_RIGHT)
        r.w = s0.w + dx;
    if (resize_edges & EDGE_LEFT)
        r.w = s0.w - dx;
    if (resize_edges & EDGE_BOTTOM)
        r.h = s0.h + dy;
    if (resize_edges & EDGE_TOP)
        r.h = s0.h - dy;
    if (r.w < min_w) r.w = min_w;
    if (r.h < min_h) r.h = min_h;
    if (r.w > W) r.w = W;
    if (r.h > H - TASKBAR_H) r.h = H - TASKBAR_H;
    if (resize_edges & EDGE_LEFT)           /* ciagniemy lewa krawedz - prawa stoi w miejscu */
        r.x = s0.x + s0.w - r.w;
    if (resize_edges & EDGE_TOP) {
        r.y = s0.y + s0.h - r.h;
        if (r.y < 0) {
            r.h += r.y;
            r.y = 0;
        }
    }
    if (r.x != resizing->r.x || r.y != resizing->r.y || r.w != resizing->r.w || r.h != resizing->r.h) {
        invalidate_window(resizing);
        resizing->r = r;
        invalidate_window(resizing);
    }
}

static void end_resize(void)
{
    if (resizing && resizing->app->resize)
        resizing->app->resize(resizing);    /* np. terminal przelicza liczbe wierszy */
    resizing = 0;
}

/* Jak w Windows: przeciagniecie powiekszonego okna przywraca je i od razu przesuwa */
static void do_unsnap(int x, int y)
{
    struct window *w = unsnap;
    int dx = x - unsnap_x, dy = y - unsnap_y;
    if (dx * dx + dy * dy < 36)
        return;
    int rel = W ? (x * 1000 / W) : 0;       /* gdzie w poziomie chwycilismy pasek tytulu */
    toggle_maximize(w);
    invalidate_window(w);
    w->r.x = x - w->r.w * rel / 1000;
    w->r.y = 0;
    invalidate_window(w);
    unsnap = 0;
    dragging = w;
    drag_dx = x - w->r.x;
    drag_dy = y - w->r.y;
}

static void on_press(int x, int y)
{
    uint32_t now = timer_ticks();
    int dbl = now - last_click_tick < DOUBLE_CLICK_TICKS &&
              x - last_click_x < 6 && last_click_x - x < 6 &&
              y - last_click_y < 6 && last_click_y - y < 6;
    last_click_tick = dbl ? 0 : now;        /* trzecie klikniecie nie jest znow podwojne */
    last_click_x = x;
    last_click_y = y;

    if (menu_open) {
        int item = menu_item_at(x, y);
        struct rect m = menu_rect(), s = start_rect();
        set_menu(0);
        if (rect_contains(&m, x, y)) {
            if (item >= 0)
                menu_action(item);
            return;
        }
        if (rect_contains(&s, x, y))
            return;                         /* Start zamyka otwarte menu */
    }

    if (y >= H - TASKBAR_H) {               /* pasek zadan */
        struct rect s = start_rect();
        if (rect_contains(&s, x, y)) {
            set_menu(1);
            return;
        }
        int n = 0;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (!windows[i].used)
                continue;
            struct rect b = task_button(n++);
            if (!rect_contains(&b, x, y))
                continue;
            if (focused() == &windows[i])
                minimize(&windows[i]);      /* jak w Windows: klik w aktywne okno je chowa */
            else
                focus(&windows[i]);
        }
        return;
    }

    for (int i = count - 1; i >= 0; i--) {  /* od okna na wierzchu */
        struct window *w = order[i];
        if (w->minimized || !rect_contains(&w->r, x, y))
            continue;
        focus(w);
        int edges = edges_at(w, x, y);
        if (edges) {                        /* chwycona krawedz albo naroznik */
            resizing = w;
            resize_edges = edges;
            resize_start = w->r;
            resize_mx = x;
            resize_my = y;
            return;
        }
        struct rect c = close_rect(w), m = minimize_rect(w), mx = maximize_rect(w);
        if (rect_contains(&c, x, y)) {
            gui_close(w);
        } else if (rect_contains(&m, x, y)) {
            minimize(w);
        } else if (rect_contains(&mx, x, y)) {
            toggle_maximize(w);
        } else if (y < w->r.y + GUI_TITLE_H) {
            if (dbl) {                      /* podwojne klikniecie w pasek tytulu */
                toggle_maximize(w);
            } else if (w->maximized) {      /* przesuniecie myszy odklei je od ekranu */
                unsnap = w;
                unsnap_x = x;
                unsnap_y = y;
            } else {
                dragging = w;
                drag_dx = x - w->r.x;
                drag_dy = y - w->r.y;
            }
        } else if (w->app->click) {
            struct rect client = client_rect(w);
            if (rect_contains(&client, x, y))
                w->app->click(w, x - client.x, y - client.y, dbl);
        }
        return;
    }

    int hit = -1;                           /* pulpit */
    for (int i = 0; i < ICON_COUNT; i++) {
        struct rect r = icon_rect(i);
        if (rect_contains(&r, x, y))
            hit = i;
    }
    select_icon(hit);
    if (hit >= 0 && dbl)
        launch(hit);
}

static void on_mouse_move(int x, int y)
{
    if (resizing)
        do_resize(x, y);
    if (unsnap)
        do_unsnap(x, y);
    if (dragging) {
        int nx = x - drag_dx, ny = y - drag_dy;
        if (ny < 0)
            ny = 0;
        if (ny > H - TASKBAR_H - GUI_TITLE_H)
            ny = H - TASKBAR_H - GUI_TITLE_H;
        if (nx > W - 60)
            nx = W - 60;
        if (nx + dragging->r.w < 60)
            nx = 60 - dragging->r.w;
        if (nx != dragging->r.x || ny != dragging->r.y) {
            invalidate_window(dragging);
            dragging->r.x = nx;
            dragging->r.y = ny;
            invalidate_window(dragging);
        }
    }
    if (menu_open) {
        int hover = menu_item_at(x, y);
        if (hover != menu_hover) {
            menu_hover = hover;
            invalidate(menu_rect());
        }
    }
}

/* Kolko myszy trafia do okna pod kursorem (jak w Windows 10) */
static void on_wheel(int x, int y, int delta)
{
    for (int i = count - 1; i >= 0; i--) {
        struct window *w = order[i];
        if (w->minimized || !rect_contains(&w->r, x, y))
            continue;
        if (w->app->scroll)
            w->app->scroll(w, delta);
        return;
    }
}

static void on_key(int key)
{
    if (key == KEY_MOUSE_LEFT)
        return;                             /* klikniecia obslugujemy z kolejki myszy */
    struct window *w = focused();
    if (key == 27 && menu_open) {           /* Esc zamyka menu */
        set_menu(0);
        return;
    }
    if (key == 27 && w && !w->app->wants_escape) {   /* ...albo okno na wierzchu */
        gui_close(w);
        return;
    }
    if (w && w->app->key)
        w->app->key(w, key);
}

/* ---------- petla glowna ---------- */

static void render_background(void)
{
    gfx_target_background(1);
    gfx_set_clip(0);
    const struct wallpaper *wp = &gui_wallpapers[gui_get_settings()->wallpaper];
    gfx_gradient(0, 0, W, H, wp->top, wp->bottom);
    if (gui_get_settings()->watermark) {
        const char *mark = OS_NAME;
        int scale = W >= 1600 ? 10 : 6;
        int tw = gfx_text_width(mark) * scale;
        uint32_t color = gfx_mix(gfx_mix(wp->top, wp->bottom, 1, 2), 0xFFFFFF, 1, 9);
        gfx_text_scaled((W - tw) / 2, (H - TASKBAR_H - 16 * scale) / 2, mark, color, scale);
    }
    gfx_target_background(0);
}

void gui_apply_settings(void)
{
    render_background();
    invalidate_all();
    gui_settings_save();
}

static void compose(void)
{
    struct rect screen = { 0, 0, W, H };
    struct rect r = intersect(dirty, screen);
    dirty.w = dirty.h = 0;
    if (r.w <= 0 || r.h <= 0)
        return;
    gfx_set_clip(&r);
    gfx_copy_background();
    draw_icons();
    struct window *top = focused();
    for (int i = 0; i < count; i++)
        if (!order[i]->minimized)
            draw_window(order[i], order[i] == top);
    draw_taskbar();
    if (menu_open)
        draw_menu();
    gfx_set_clip(0);
    gfx_present(&r);
}

int gui_run(void)
{
    if (!gfx_begin())
        return 0;
    W = gfx_width();
    H = gfx_height();
    gui_settings_load();
    render_background();
    invalidate_all();

    struct mouse_event ev;
    while (mouse_poll_event(&ev))           /* stare klikniecia i klawisze nie dotycza pulpitu */
        ;
    while (keyboard_poll() >= 0)
        ;
    quit = 0;
    set_menu(0);
    dragging = 0;

    uint8_t buttons = 0;
    int mx, my, last_mx = -1, last_my = -1;
    uint32_t last_clock = (uint32_t)-1;

    while (!quit) {
        while (mouse_poll_event(&ev)) {
            if ((ev.buttons & MOUSE_LEFT) && !(buttons & MOUSE_LEFT))
                on_press(ev.x, ev.y);
            if (!(ev.buttons & MOUSE_LEFT)) {
                dragging = 0;
                unsnap = 0;
                end_resize();
            }
            buttons = ev.buttons;
        }

        mouse_position(&mx, &my);
        if (mx != last_mx || my != last_my) {
            on_mouse_move(mx, my);
            last_mx = mx;
            last_my = my;
        }

        int wheel = mouse_wheel();
        if (wheel)
            on_wheel(mx, my, wheel);

        int key;
        while ((key = keyboard_poll()) >= 0)
            on_key(key);

        /* Zegar na pasku i okna "na zywo" - 5 razy na sekunde sprawdzamy zmiane */
        uint32_t tick = timer_ticks() / (TIMER_HZ / 5);
        if (tick != last_clock) {
            last_clock = tick;
            struct rect clock = { W - 132, H - TASKBAR_H, 132, TASKBAR_H };
            invalidate(clock);
            if (tick % 5 == 0)
                for (int i = 0; i < count; i++)
                    if (order[i]->app->live)
                        gui_redraw(order[i]);
        }

        for (int i = 0; i < MAX_WINDOWS; i++)   /* programy dzialajace w tle */
            if (windows[i].used && windows[i].app->tick)
                windows[i].app->tick(&windows[i]);

        if (dirty.w > 0 && dirty.h > 0)
            compose();
        gfx_cursor_move(mx, my);
        cpu_idle();                         /* terminale dzialaja, potem sen do przerwania */
    }

    for (int i = MAX_WINDOWS - 1; i >= 0; i--)   /* tryb tekstowy: zamykamy pulpit */
        if (windows[i].used)
            gui_close(&windows[i]);
    gfx_end();
    return 1;
}

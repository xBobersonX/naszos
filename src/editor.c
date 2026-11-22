/* Edytor tekstu w stylu nano. Tekst trzymamy jako tablice linii; kazda
 * linia to osobny bufor z kmalloc, ktory rosnie w miare pisania. Po kazdym
 * klawiszu przerysowujemy caly ekran - to tylko 2000 znakow w pamieci VGA,
 * wiec jest to natychmiastowe, a kod pozostaje prosty.
 *
 * Uklad ekranu:  wiersz 0      - pasek statusu systemu (zegar)
 *                wiersze 1-23  - tekst
 *                wiersz 24     - pasek edytora (nazwa pliku, pozycja, skroty) */

#include "editor.h"
#include "vga.h"
#include "keyboard.h"
#include "heap.h"
#include "string.h"
#include "storage.h"
#include "mouse.h"

#define TEXT_TOP  VGA_TOP
#define TEXT_ROWS (VGA_HEIGHT - VGA_TOP - 1)
#define BAR_ROW   (VGA_HEIGHT - 1)
#define TAB_WIDTH 4

#define TEXT_FG VGA_LIGHT_GREY
#define TEXT_BG VGA_BLACK
#define BAR_FG  VGA_BLACK
#define BAR_BG  VGA_LIGHT_CYAN

struct line {
    char *s;
    uint32_t len, cap;
};

struct editor {
    struct line *lines;
    uint32_t count, cap;
    uint32_t cx, cy;             /* kursor: kolumna i numer linii w tekscie */
    uint32_t row_off, col_off;   /* przewiniecie ekranu */
    int modified;
    int quit_armed;              /* Ctrl+Q wcisniete raz przy niezapisanych zmianach */
    struct fs_node *cwd;
    const char *path;
    char msg[VGA_MAX_COLS];           /* komunikat na pasku, znika po nastepnym klawiszu */
};

/* ---------- operacje na liniach ---------- */

static int line_reserve(struct line *l, uint32_t need)
{
    if (need <= l->cap)
        return 1;
    uint32_t cap = l->cap ? l->cap * 2 : 16;
    if (cap < need)
        cap = need;
    char *s = kmalloc(cap);
    if (!s)
        return 0;
    memcpy(s, l->s, l->len);
    kfree(l->s);
    l->s = s;
    l->cap = cap;
    return 1;
}

static int line_append(struct line *l, const char *s, uint32_t n)
{
    if (!line_reserve(l, l->len + n))
        return 0;
    memcpy(l->s + l->len, s, n);
    l->len += n;
    return 1;
}

/* Wstawia pusta linie na pozycji idx */
static int insert_line(struct editor *e, uint32_t idx)
{
    if (e->count == e->cap) {
        uint32_t cap = e->cap ? e->cap * 2 : 32;
        struct line *l = kmalloc(cap * sizeof(*l));
        if (!l)
            return 0;
        memcpy(l, e->lines, e->count * sizeof(*l));
        kfree(e->lines);
        e->lines = l;
        e->cap = cap;
    }
    memmove(&e->lines[idx + 1], &e->lines[idx], (e->count - idx) * sizeof(struct line));
    memset(&e->lines[idx], 0, sizeof(struct line));
    e->count++;
    return 1;
}

static void remove_line(struct editor *e, uint32_t idx)
{
    kfree(e->lines[idx].s);
    memmove(&e->lines[idx], &e->lines[idx + 1], (e->count - idx - 1) * sizeof(struct line));
    e->count--;
}

static void free_all(struct editor *e)
{
    for (uint32_t i = 0; i < e->count; i++)
        kfree(e->lines[i].s);
    kfree(e->lines);
}

/* ---------- pomocnicze ---------- */

static void set_msg(struct editor *e, const char *a, const char *b)
{
    size_t n = 0;
    for (; a && *a && n < sizeof(e->msg) - 1; a++)
        e->msg[n++] = *a;
    for (; b && *b && n < sizeof(e->msg) - 1; b++)
        e->msg[n++] = *b;
    e->msg[n] = '\0';
}

static size_t put_str(char *dst, size_t pos, const char *s)
{
    while (*s && pos < VGA_WIDTH)
        dst[pos++] = *s++;
    return pos;
}

static size_t put_uint(char *dst, size_t pos, uint32_t v)
{
    char tmp[11];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n-- && pos < VGA_WIDTH)
        dst[pos++] = tmp[n];
    return pos;
}

static struct line *cur(struct editor *e)
{
    return &e->lines[e->cy];
}

/* ---------- wczytywanie i zapis ---------- */

static int load(struct editor *e)
{
    struct fs_node *f;
    if (!insert_line(e, 0))
        return 0;
    if (fs_lookup(e->cwd, e->path, &f) != FS_OK) {
        set_msg(e, "Nowy plik - zostanie utworzony przy zapisie (Ctrl+S)", 0);
        return 1;
    }
    for (uint32_t i = 0; i < f->size; i++) {
        char c = f->data[i];
        if (c == '\n') {
            if (i + 1 < f->size && !insert_line(e, e->count))   /* koncowy '\n' nie tworzy linii */
                return 0;
        } else if (!line_append(&e->lines[e->count - 1], &c, 1)) {
            return 0;
        }
    }
    return 1;
}

static void save(struct editor *e)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < e->count; i++)
        total += e->lines[i].len + 1;
    if (e->count == 1 && e->lines[0].len == 0)
        total = 0;                      /* pusty plik */

    char *buf = 0;
    if (total && !(buf = kmalloc(total))) {
        set_msg(e, "Blad: brak pamieci", 0);
        return;
    }
    uint32_t pos = 0;
    for (uint32_t i = 0; i < e->count && total; i++) {
        memcpy(buf + pos, e->lines[i].s, e->lines[i].len);
        pos += e->lines[i].len;
        buf[pos++] = '\n';
    }

    struct fs_node *f;
    int err = fs_lookup(e->cwd, e->path, &f);
    if (err == FS_ENOENT)
        err = fs_create(e->cwd, e->path, 0, &f);
    if (err == FS_OK)
        err = fs_write(f, buf, total, 0);
    kfree(buf);

    if (err) {
        set_msg(e, "Blad zapisu: ", fs_strerror(err));
        return;
    }
    e->modified = 0;

    err = storage_sync_if_dirty();       /* od razu na dysk, nie dopiero po wyjsciu */
    if (err) {
        set_msg(e, "Zapisano w pamieci, ale blad dysku: ", storage_strerror(err));
        return;
    }
    char m[VGA_MAX_COLS] = "Zapisano ";
    size_t n = put_uint(m, strlen(m), total);
    n = put_str(m, n, " B");
    m[n] = '\0';
    set_msg(e, m, 0);
}

/* ---------- edycja ---------- */

static int insert_char(struct editor *e, char c)
{
    struct line *l = cur(e);
    if (!line_reserve(l, l->len + 1)) {
        set_msg(e, "Blad: brak pamieci", 0);
        return 0;
    }
    memmove(l->s + e->cx + 1, l->s + e->cx, l->len - e->cx);
    l->s[e->cx++] = c;
    l->len++;
    e->modified = 1;
    return 1;
}

static void insert_newline(struct editor *e)
{
    if (!insert_line(e, e->cy + 1)) {
        set_msg(e, "Blad: brak pamieci", 0);
        return;
    }
    struct line *l = cur(e);                  /* po insert_line tablica mogla sie przeniesc */
    struct line *next = &e->lines[e->cy + 1];
    if (!line_append(next, l->s + e->cx, l->len - e->cx)) {
        remove_line(e, e->cy + 1);
        set_msg(e, "Blad: brak pamieci", 0);
        return;
    }
    l->len = e->cx;
    e->cy++;
    e->cx = 0;
    e->modified = 1;
}

/* Dolacza linie idx+1 na koniec linii idx */
static void join_with_next(struct editor *e, uint32_t idx)
{
    struct line *next = &e->lines[idx + 1];
    if (!line_append(&e->lines[idx], next->s, next->len)) {
        set_msg(e, "Blad: brak pamieci", 0);
        return;
    }
    remove_line(e, idx + 1);
    e->modified = 1;
}

static void backspace(struct editor *e)
{
    struct line *l = cur(e);
    if (e->cx > 0) {
        memmove(l->s + e->cx - 1, l->s + e->cx, l->len - e->cx);
        l->len--;
        e->cx--;
        e->modified = 1;
    } else if (e->cy > 0) {
        e->cx = e->lines[e->cy - 1].len;
        e->cy--;
        join_with_next(e, e->cy);
    }
}

static void delete_char(struct editor *e)
{
    struct line *l = cur(e);
    if (e->cx < l->len) {
        memmove(l->s + e->cx, l->s + e->cx + 1, l->len - e->cx - 1);
        l->len--;
        e->modified = 1;
    } else if (e->cy + 1 < e->count) {
        join_with_next(e, e->cy);
    }
}

static void move_cursor(struct editor *e, int key)
{
    switch (key) {
    case KEY_LEFT:
        if (e->cx > 0)
            e->cx--;
        else if (e->cy > 0)
            e->cx = e->lines[--e->cy].len;
        break;
    case KEY_RIGHT:
        if (e->cx < cur(e)->len)
            e->cx++;
        else if (e->cy + 1 < e->count) {
            e->cy++;
            e->cx = 0;
        }
        break;
    case KEY_UP:   if (e->cy > 0) e->cy--; break;
    case KEY_DOWN: if (e->cy + 1 < e->count) e->cy++; break;
    case KEY_PGUP: e->cy = e->cy > TEXT_ROWS ? e->cy - TEXT_ROWS : 0; break;
    case KEY_PGDN:
        e->cy += TEXT_ROWS;
        if (e->cy >= e->count)
            e->cy = e->count - 1;
        break;
    case KEY_HOME: e->cx = 0; break;
    case KEY_END:  e->cx = cur(e)->len; break;
    }
    if (e->cx > cur(e)->len)               /* w krotszej linii kursor idzie na jej koniec */
        e->cx = cur(e)->len;
}

/* Klikniecie mysza w tekst przenosi tam kursor */
static void click(struct editor *e)
{
    unsigned x, y;
    mouse_last_click(&x, &y);
    if (y < TEXT_TOP || y >= TEXT_TOP + TEXT_ROWS)
        return;
    uint32_t line = e->row_off + (y - TEXT_TOP);
    e->cy = line < e->count ? line : e->count - 1;
    e->cx = e->col_off + x;
    if (e->cx > cur(e)->len)
        e->cx = cur(e)->len;
}

/* ---------- rysowanie ---------- */

static void scroll_to_cursor(struct editor *e)
{
    if (e->cy < e->row_off)
        e->row_off = e->cy;
    if (e->cy >= e->row_off + TEXT_ROWS)
        e->row_off = e->cy - TEXT_ROWS + 1;
    if (e->cx < e->col_off)
        e->col_off = e->cx;
    if (e->cx >= e->col_off + VGA_WIDTH)
        e->col_off = e->cx - VGA_WIDTH + 1;
}

static void draw(struct editor *e)
{
    scroll_to_cursor(e);

    for (unsigned r = 0; r < TEXT_ROWS; r++) {
        uint32_t idx = e->row_off + r;
        for (unsigned x = 0; x < VGA_WIDTH; x++) {
            if (idx < e->count) {
                const struct line *l = &e->lines[idx];
                uint32_t col = e->col_off + x;
                vga_put_at(x, TEXT_TOP + r, col < l->len ? l->s[col] : ' ', TEXT_FG, TEXT_BG);
            } else {
                /* "~" oznacza wiersze za koncem pliku, jak w vi */
                vga_put_at(x, TEXT_TOP + r, x == 0 ? '~' : ' ', VGA_DARK_GREY, TEXT_BG);
            }
        }
    }

    /* Pasek: " plik.txt [zmieniony]   ^S Zapisz  ^Q Wyjdz   Ln 3/10 Kol 5 "
     * albo, gdy jest komunikat, sam komunikat na zoltym tle. */
    char bar[VGA_MAX_COLS + 1];
    memset(bar, ' ', VGA_WIDTH);
    bar[VGA_WIDTH] = '\0';

    if (e->msg[0]) {
        put_str(bar, 1, e->msg);
    } else {
        size_t n = put_str(bar, 1, e->path);
        if (e->modified)
            n = put_str(bar, n, " [zmieniony]");

        char pos[48];                    /* "Ln 4294967295/4294967295 Kol 4294967295" */
        size_t p = put_str(pos, 0, "Ln ");
        p = put_uint(pos, p, e->cy + 1);
        p = put_str(pos, p, "/");
        p = put_uint(pos, p, e->count);
        p = put_str(pos, p, " Kol ");
        p = put_uint(pos, p, e->cx + 1);
        pos[p] = '\0';
        size_t right = VGA_WIDTH - 1 - p;
        put_str(bar, right, pos);

        static const char keys[] = "^S Zapisz  ^Q Wyjdz";
        if (n + 3 + sizeof(keys) + 2 <= right)
            put_str(bar, n + 3, keys);
    }

    enum vga_color bg = e->msg[0] ? VGA_YELLOW : BAR_BG;
    for (unsigned x = 0; x < VGA_WIDTH; x++)
        vga_put_at(x, BAR_ROW, bar[x], BAR_FG, bg);

    vga_set_pos(e->cx - e->col_off, TEXT_TOP + (e->cy - e->row_off));
    vga_mouse_refresh();                 /* przerysowany ekran zamazal kursor myszy */
}

/* ---------- petla glowna ---------- */

void editor_run(struct fs_node *cwd, const char *path)
{
    struct editor e;
    memset(&e, 0, sizeof(e));
    e.cwd = cwd;
    e.path = path;

    if (!load(&e)) {
        free_all(&e);
        kprintf("edit: brak pamieci na wczytanie pliku\n");
        return;
    }

    /* Kopia ekranu przed edytorem - 150 KB, wiec z kmalloc (osobna dla
     * kazdego edytora - moze ich dzialac kilka w roznych oknach) */
    struct vga_state *saved = kmalloc(sizeof(*saved));
    if (!saved) {
        free_all(&e);
        kprintf("edit: brak pamieci\n");
        return;
    }
    vga_save(saved);
    for (;;) {
        draw(&e);
        int key = keyboard_getkey();
        e.msg[0] = '\0';

        if (key == KEY_CTRL('q')) {
            if (!e.modified || e.quit_armed)
                break;
            e.quit_armed = 1;
            set_msg(&e, "Niezapisane zmiany! ^Q = wyjdz bez zapisu, ^S = zapisz", 0);
            continue;
        }
        e.quit_armed = 0;

        switch (key) {
        case KEY_CTRL('s'): save(&e); break;
        case KEY_MOUSE_LEFT: click(&e); break;
        case '\n':          insert_newline(&e); break;
        case '\b':          backspace(&e); break;
        case KEY_DELETE:    delete_char(&e); break;
        case '\t':
            while (insert_char(&e, ' ') && e.cx % TAB_WIDTH)
                ;
            break;
        default:
            if (key >= ' ' && key < 127)
                insert_char(&e, (char)key);
            else
                move_cursor(&e, key);
            break;
        }
    }

    free_all(&e);
    vga_restore(saved);
    kfree(saved);
}

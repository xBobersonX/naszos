/* Przegladarka internetowa NaszOS - okno na pulpicie.
 *
 * Jak to dziala:
 *  1. Adres z paska (albo klikniety link) dostaje osobne zadanie (watek),
 *     ktore pobiera strone przez http.c - dzieki temu pulpit sie nie
 *     zawiesza, gdy serwer dlugo odpowiada.
 *  2. html.c zamienia HTML na "klocki": slowa, akapity, punkty, linki.
 *  3. Uklad (layout): klocki ustawiamy jak slowa w zeszycie - od lewej do
 *     prawej, a gdy slowo sie nie miesci, przechodzimy do nowej linii.
 *     Zapamietujemy, gdzie stoi kazde slowo - klikniecie w niebieskie
 *     slowo otwiera jego link.
 *
 * JavaScript (src/js_*.c): skrypty strony wykonuja sie w trakcie ladowania
 * (w zadaniu pobierajacym), a potem - klikniecia, klawisze i timery - w
 * petli pulpitu. Gdy skrypt zmieni strone, liczymy od nowa style i uklad.
 *
 * Strony https:// otwieramy wlasnym szyfrowaniem TLS 1.3 (src/tls.c). Gdy
 * sie nie uda (np. serwer zna tylko starsze TLS 1.2), plan B to FrogFind:
 * serwer, ktory pobiera strone za nas i oddaje jej prosta wersje zwyklym
 * http. Tekst wpisany bez kropki (np. "minecraft") to wyszukiwanie w Google. */

#include "gui.h"
#include "gfx.h"
#include "heap.h"
#include "string.h"
#include "thread.h"
#include "vga.h"
#include "keyboard.h"
#include "http.h"
#include "html.h"
#include "net.h"
#include "image.h"
#include "font.h"
#include "utf8.h"
#include "dom.h"
#include "css.h"
#include "layout.h"
#include "js.h"
#include "io.h"

#define TOOLBAR_H   38
#define STATUS_H    22
#define PAD         14
#define SCROLLBAR_W 14
#define LINE_H      24               /* do przewijania: tyle ma linia tekstu */
#define LINE_GAP    6                /* odstep miedzy liniami */
#define CONTENT_MAX 940              /* najszersza kolumna tekstu - dluzsze linie zle sie czyta */
#define HIST_MAX    16
#define IMAGES_MAX  80               /* tyle obrazkow z jednej strony pobieramy */

#define START_URL   "naszos:start"
#define DEMO_URL    "naszos:js"
#define PROXY_URL   "http://frogfind.com/read.php?a="
#define SEARCH_URL  "http://www.google.com/search?hl=pl&q="
#define WIKI_RENDER "?action=render"     /* Wikipedia: sam artykul, bez menu i 300 jezykow */

#define COLOR_TEXT    0x202122
#define COLOR_MUTED   0x6B7280
#define COLOR_LINK    0x3366CC
#define COLOR_H1      0x000000
#define COLOR_H2      0x101418
#define COLOR_IMAGE   0x9CA3AF
#define COLOR_ERROR   0xB91C1C

static const char start_page[] =
    "<title>Strona startowa</title>"
    "<style>"
    "body{max-width:900px;margin:0 auto;padding:6px 24px 20px;color:#202122;line-height:1.5}"
    "h1{font-size:28px;font-weight:normal;border-bottom:1px solid #a2a9b1;margin:14px 0 10px}"
    "h2{font-size:20px;margin:0 0 6px}"
    "a{color:#3366cc;text-decoration:none}"
    ".box{background:#f8f9fa;border:1px solid #c8ccd1;padding:10px 16px;margin:12px 0}"
    ".box ul{margin:4px 0;padding-left:22px}"
    ".note{background:#fef6e7;border:1px solid #fc3;padding:8px 14px}"
    "</style>"
    "<h1>Witaj w internecie!</h1>"
    "<p>To jest przeglądarka <b>NaszOS</b> – napisana od zera, razem z całym systemem: "
    "sterownikiem karty sieciowej, TCP/IP, szyfrowaniem HTTPS, dekoderami obrazków "
    "i układaniem tekstu na ekranie.</p>"
    "<p>Wpisz adres u góry (np. <b>example.com</b>) albo cokolwiek, czego szukasz "
    "(np. <b>minecraft</b>) – wtedy zapytamy Google.</p>"
    "<div class=\"box\"><h2>Strony do odwiedzenia</h2><ul>"
    "<li><a href=\"https://pl.wikipedia.org/wiki/Kot_domowy\">Wikipedia: Kot domowy</a> – artykuł ze zdjęciami, szyfrowany naszym TLS 1.3</li>"
    "<li><a href=\"http://info.cern.ch/\">info.cern.ch</a> – pierwsza strona WWW na świecie (1991)</li>"
    "<li><a href=\"http://www.google.com/search?hl=pl&amp;q=minecraft\">Google: minecraft</a> – wyniki wyszukiwania</li>"
    "<li><a href=\"http://68k.news/\">68k.news</a> – wiadomości ze świata dla starych komputerów</li>"
    "<li><a href=\"http://frogfind.com/\">FrogFind</a> – wyszukiwarka dla prostych przeglądarek</li>"
    "<li><a href=\"http://textfiles.com/100/\">textfiles.com</a> – teksty z czasów BBS-ów (lata 80.)</li>"
    "<li><a href=\"http://example.com/\">example.com</a> – najprostsza strona w internecie</li>"
    "<li><a href=\"naszos:js\">Pokaz JavaScriptu</a> – licznik, kolory, zegar, kalkulator i gra w węża</li>"
    "</ul>"
    "</div><div class=\"box\"><h2>Jak to działa?</h2><ul>"
    "<li><b>DNS</b> zamienia nazwę (google.com) na adres IP – jak książka telefoniczna.</li>"
    "<li><b>TCP</b> łączy nas z serwerem i pilnuje, żeby żaden kawałek strony nie zginął.</li>"
    "<li><b>TLS</b> szyfruje strony https: X25519 uzgadnia sekret, ChaCha20 szyfruje, "
    "Poly1305 pilnuje, żeby nikt nic nie zmienił.</li>"
    "<li><b>HTTP</b> to prośba: <i>„GET / – daj mi stronę główną”</i>.</li>"
    "<li><b>HTML</b> to tekst strony ze znacznikami, np. &lt;a href=...&gt; to link.</li>"
    "</ul>"
    "</div><div class=\"box\"><h2>Sterowanie</h2><ul>"
    "<li>Klik w <b>niebieski link</b> – otwiera stronę</li>"
    "<li>Kółko myszy, strzałki, PgUp/PgDn, spacja – przewijanie</li>"
    "<li>Backspace albo przycisk ze strzałką – wstecz, domek – ta strona</li>"
    "<li>Pisanie od razu trafia do paska adresu, Enter – otwórz</li>"
    "</ul>"
    "</div><div class=\"note\"><b>Czego jeszcze nie umiemy:</b> "
    "wielkich aplikacji w JavaScripcie (YouTube, Google Docs – nasz silnik JS radzi sobie z prostymi "
    "skryptami) i sprawdzania certyfikatów – "
    "połączenie https jest szyfrowane, ale NaszOS nie sprawdza, czy serwer jest tym, za kogo się "
    "podaje. Dlatego: czytać strony – tak, logować się i wpisywać hasła – nie.</div>";

/* Pokaz JavaScriptu - strona wbudowana w przegladarke */
static const char demo_page[] =
    "<title>Pokaz JavaScriptu</title>"
    "<style>"
    "body{max-width:900px;margin:0 auto;padding:6px 24px 20px;color:#202122;line-height:1.5}"
    "h1{font-size:28px;font-weight:normal;border-bottom:1px solid #a2a9b1;margin:14px 0 10px}"
    "h2{font-size:19px;margin:0 0 8px}"
    ".box{background:#f8f9fa;border:1px solid #c8ccd1;padding:10px 16px;margin:12px 0}"
    "button{margin-right:6px}"
    ".kolor{background:#dbeafe;border:2px solid #3b82f6;padding:14px;margin:6px 0 10px}"
    "#zegar{font-size:30px;font-weight:bold;color:#1d4ed8}"
    ".row{display:flex}"
    ".c{width:16px;height:16px;margin:1px;background:#e5e7eb}"
    ".s{background:#16a34a}.f{background:#dc2626}"
    "#punkty{font-weight:bold}"
    "</style>"
    "<h1>JavaScript w NaszOS</h1>"
    "<p>Wszystko na tej stronie robi <b>JavaScript</b> – program w przeglądarce. "
    "Nasz własny silnik JS czyta ten program, a potem reaguje na kliknięcia, klawisze i zegar.</p>"
    "<div class=\"box\"><h2>1. Licznik kliknięć</h2>"
    "<button id=\"btn\">Kliknij mnie!</button> <span id=\"cnt\">Jeszcze nikt nie kliknął.</span></div>"
    "<div class=\"box\"><h2>2. Kolory</h2><div id=\"kolor\" class=\"kolor\">Jestem pudełkiem – wybierz mi kolor</div>"
    "<button data-k=\"#fecaca\">Czerwony</button><button data-k=\"#bbf7d0\">Zielony</button>"
    "<button data-k=\"#fef08a\">Żółty</button><button data-k=\"#e9d5ff\">Fioletowy</button></div>"
    "<div class=\"box\"><h2>3. Zegar (setInterval co sekundę)</h2><div id=\"zegar\">--:--:--</div></div>"
    "<div class=\"box\"><h2>4. Kalkulator</h2>"
    "<p>Kliknij w pole i wpisz liczbę (Backspace kasuje).</p>"
    "<input id=\"a\" value=\"12\" size=\"8\"> + <input id=\"b\" value=\"30\" size=\"8\"> "
    "<button id=\"licz\">=</button> <b id=\"wynik\"></b></div>"
    "<div class=\"box\"><h2>5. Wąż</h2>"
    "<p>Spacja – start i pauza, strzałki – sterowanie. Zjadaj czerwone kwadraty!</p>"
    "<div id=\"plansza\"></div><p id=\"punkty\">Punkty: 0 – naciśnij spację</p></div>"
    "<script>"
    "var n = 0;"
    "document.getElementById('btn').addEventListener('click', function () {"
    "  n++;"
    "  document.getElementById('cnt').textContent = 'Kliknięto ' + n + (n === 1 ? ' raz' : ' razy') + '.';"
    "});"
    "document.querySelectorAll('[data-k]').forEach(function (b) {"
    "  b.addEventListener('click', function () {"
    "    var box = document.getElementById('kolor');"
    "    box.style.backgroundColor = b.dataset.k;"
    "    box.textContent = 'Teraz jestem: ' + b.textContent.toLowerCase();"
    "  });"
    "});"
    "function tick() { document.getElementById('zegar').textContent = new Date().toLocaleTimeString('pl-PL'); }"
    "tick(); setInterval(tick, 1000);"
    "document.getElementById('licz').onclick = function () {"
    "  var a = parseFloat(document.getElementById('a').value), b = parseFloat(document.getElementById('b').value);"
    "  document.getElementById('wynik').textContent = isNaN(a + b) ? 'to nie są liczby!' : String(a + b);"
    "};"
    "const W = 20, H = 12, cells = [], plansza = document.getElementById('plansza');"
    "for (let y = 0; y < H; y++) {"
    "  const row = document.createElement('div'); row.className = 'row';"
    "  for (let x = 0; x < W; x++) { const c = document.createElement('div'); c.className = 'c'; row.appendChild(c); cells.push(c); }"
    "  plansza.appendChild(row);"
    "}"
    "let snake = null, dir, nextDir, food, timer = null, points = 0, over = true;"
    "const cell = p => cells[p.y * W + p.x];"
    "function show(msg) { document.getElementById('punkty').textContent = 'Punkty: ' + points + (msg ? ' – ' + msg : ''); }"
    "function placeFood() {"
    "  do { food = { x: Math.floor(Math.random() * W), y: Math.floor(Math.random() * H) }; }"
    "  while (snake.some(p => p.x === food.x && p.y === food.y));"
    "  cell(food).className = 'c f';"
    "}"
    "function reset() {"
    "  cells.forEach(c => c.className = 'c');"
    "  snake = [{ x: 5, y: 6 }, { x: 4, y: 6 }, { x: 3, y: 6 }];"
    "  snake.forEach(p => cell(p).className = 'c s');"
    "  dir = nextDir = { x: 1, y: 0 }; points = 0; over = false; placeFood();"
    "}"
    "function step() {"
    "  dir = nextDir;"
    "  const head = { x: (snake[0].x + dir.x + W) % W, y: (snake[0].y + dir.y + H) % H };"
    "  if (snake.some(p => p.x === head.x && p.y === head.y)) {"
    "    clearInterval(timer); timer = null; over = true; show('koniec gry! Spacja = nowa gra'); return;"
    "  }"
    "  snake.unshift(head); cell(head).className = 'c s';"
    "  if (head.x === food.x && head.y === food.y) { points++; show(''); placeFood(); }"
    "  else cell(snake.pop()).className = 'c';"
    "}"
    "document.addEventListener('keydown', function (e) {"
    "  const d = { ArrowUp: { x: 0, y: -1 }, ArrowDown: { x: 0, y: 1 }, ArrowLeft: { x: -1, y: 0 }, ArrowRight: { x: 1, y: 0 } }[e.key];"
    "  if (d && snake) { if (d.x !== -dir.x || d.y !== -dir.y) nextDir = d; e.preventDefault(); }"
    "  else if (e.key === ' ') {"
    "    e.preventDefault();"
    "    if (timer) { clearInterval(timer); timer = null; show('pauza'); return; }"
    "    if (over) reset();"
    "    timer = setInterval(step, 150); show('');"
    "  }"
    "});"
    "</script>";

/* Pobieranie w tle - osobne zadanie z wlasna (niewidoczna) konsola */
struct job {
    char url[URL_MAX];
    struct console *con;
    volatile int done;
    int orphan;                  /* okno zamkniete albo nowa strona - wynik do wyrzucenia */
    int push;                    /* po zaladowaniu dopisac poprzednia strone do historii */
    int err;
    struct http_response resp;
    struct dom_doc *dom;
    int view_w, view_h;          /* rozmiar okna - dla regul @media i window.innerWidth */
    char status[160];
    volatile int status_changed;
    struct js_rt *js;            /* JavaScript strony (skrypty juz wykonane w trakcie ladowania) */
    struct css_ctx *css;         /* style strony - zostaja, bo skrypt moze zmienic strone */
    struct js_host host;
    int nscripts;
    char alert[300];             /* alert() w trakcie ladowania - pokazemy po zaladowaniu */
    char nav[URL_MAX];           /* location.href = ... w trakcie ladowania */
    char js_error[200];
};

/* Obrazki strony pobiera osobne zadanie - po kolei, zeby strona byla widoczna od razu */
struct imgjob {
    char **urls;                 /* pelne adresy (albo "data:...") - NULL = pomijamy */
    int first;                   /* numer pierwszego obrazka dokumentu w tej paczce */
    int n;
    struct image *out;
    volatile uint8_t *state;     /* 0 = czeka, 1 = gotowy, 2 = blad, 3 = zabrany przez okno */
    int max_w, max_h;
    struct console *con;
    volatile int done;
    int orphan;
};

struct browser {
    char url[URL_MAX];           /* biezaca strona */
    char addr[URL_MAX];          /* tekst w pasku adresu */
    unsigned addr_len, cursor;
    int addr_focus;
    struct dom_doc *dom;         /* strona jako drzewo z obliczonym wygladem CSS */
    struct page_layout lay;      /* co gdzie narysowac */
    int layout_w, content_h, scroll;
    char hist[HIST_MAX][URL_MAX];
    int nhist;
    struct job *job;
    char status[160];
    uint32_t status_color;
    int secure;                  /* biezaca strona przyszla przez nasz TLS */
    char page_status[160];       /* opis strony (bez licznika obrazkow) */
    struct image *imgs;          /* obrazki strony (tyle, ile doc->nimages) */
    int nimgs, imgs_done, imgs_total;
    int imgs_requested;          /* tyle pierwszych obrazkow juz zamowiono (skrypt moze dodac nowe) */
    struct imgjob *ij;
    struct js_rt *js;            /* JavaScript biezacej strony (albo NULL) */
    struct css_ctx *css;         /* style biezacej strony (do przeliczenia po zmianach) */
    char alert[300];             /* okienko alert() - pusty = brak */
    char pending_nav[URL_MAX];   /* skrypt kazal przejsc na inna strone */
    int pending_back;
    int nscripts;
    unsigned canvas_gen;         /* ostatnio pokazany stan plocien <canvas> */
};

static void start_job(struct window *w, const char *url, int push);
static void go_back(struct window *w);

/* ---------- pomocnicze ---------- */

static void js_changes(struct window *w);

static int strncmp_ci_url(const char *url, const char *prefix)
{
    return str_starts_ci(url, prefix);
}

static struct rect back_rect(void)   { struct rect r = { 8, 6, 30, 26 }; return r; }
static struct rect home_rect(void)   { struct rect r = { 42, 6, 30, 26 }; return r; }
static struct rect go_rect(struct rect c)   { struct rect r = { c.w - 58, 6, 50, 26 }; return r; }
static struct rect addr_rect(struct rect c) { struct rect r = { 80, 6, c.w - 80 - 64, 26 }; return r; }

static struct rect view_rect(struct rect c)   /* obszar strony (bez paska przewijania) */
{
    struct rect r = { 0, TOOLBAR_H, c.w - SCROLLBAR_W, c.h - TOOLBAR_H - STATUS_H };
    return r;
}

static void set_status(struct browser *b, const char *msg, uint32_t color)
{
    ksnprintf(b->status, sizeof(b->status), "%s", msg);
    ksnprintf(b->page_status, sizeof(b->page_status), "%s", msg);
    b->status_color = color;
}

static void set_addr(struct browser *b, const char *s)
{
    ksnprintf(b->addr, sizeof(b->addr), "%s", s);
    b->addr_len = b->cursor = (unsigned)strlen(b->addr);
}

/* Adres do pokazania: dla stron przez FrogFind - oryginalny adres https */
static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* %C5%82 -> "l z kreska" (UTF-8); znaki ASCII typu %3F zostawiamy zakodowane */
static void decode_utf8_escapes(char *s)
{
    char *o = s;
    for (char *i = s; *i;) {
        int h = i[0] == '%' ? hexval(i[1]) : -1, l = h >= 0 ? hexval(i[2]) : -1;
        if (h >= 8 && l >= 0) {
            *o++ = (char)(h * 16 + l);
            i += 3;
        } else {
            *o++ = *i++;
        }
    }
    *o = '\0';
}

static void display_url(const char *url, char *out, unsigned size)
{
    if (str_starts_ci(url, PROXY_URL))
        url_decode(url + strlen(PROXY_URL), out, size);
    else
        ksnprintf(out, size, "%s", url);
    decode_utf8_escapes(out);
    unsigned l = (unsigned)strlen(out), rl = (unsigned)strlen(WIKI_RENDER);
    if (l > rl && strcmp(out + l - rl, WIKI_RENDER) == 0)
        out[l - rl] = '\0';
}

static int max_scroll(struct browser *b, struct rect c)
{
    int m = b->content_h - view_rect(c).h;
    return m > 0 ? m : 0;
}

static void scroll_by(struct window *w, int dy)
{
    struct browser *b = w->state;
    struct rect c = gui_client_size(w);
    int s = b->scroll + dy, m = max_scroll(b, c);
    if (s > m) s = m;
    if (s < 0) s = 0;
    if (s != b->scroll) {
        b->scroll = s;
        gui_redraw(w);
    }
}

/* ---------- rysowanie ---------- */

static struct rect intersect(struct rect a, struct rect b)
{
    int x1 = a.x > b.x ? a.x : b.x, y1 = a.y > b.y ? a.y : b.y;
    int x2 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y2 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    struct rect r = { x1, y1, x2 > x1 ? x2 - x1 : 0, y2 > y1 ? y2 - y1 : 0 };
    return r;
}

/* Przycisk paska narzedzi: napis ladna czcionka (UTF-8) albo znak VGA (strzalka, domek) */
static void tool_button(struct rect c, struct rect r, const char *label, int vga, int enabled)
{
    uint32_t color = enabled ? COLOR_TEXT : 0xB0B4BA;
    gfx_gradient(c.x + r.x, c.y + r.y, r.w, r.h, 0xFFFFFF, 0xE5E7EB);
    gfx_frame(c.x + r.x, c.y + r.y, r.w, r.h, 0x9CA3AF);
    if (vga) {
        gfx_text(c.x + r.x + (r.w - gfx_text_width(label)) / 2, c.y + r.y + 5, label, color);
    } else {
        unsigned n = (unsigned)strlen(label);
        font_draw(FONT_REGULAR, c.x + r.x + (r.w - font_width(FONT_REGULAR, label, n)) / 2,
                  c.y + r.y + (r.h - font_height(FONT_REGULAR)) / 2, label, n, color);
    }
}

/* Jeden element listy rysowania (layout.c) - ox, oy: gdzie na ekranie jest poczatek strony */
static void paint_item(const struct browser *b, const struct dl_item *it, int ox, int oy)
{
    int x = ox + it->x, y = oy + it->y;
    switch (it->kind) {
    case DL_RECT:
        gfx_fill(x, y, it->w, it->h, it->color);
        break;
    case DL_TEXT:
        if (it->font == FONT_MONO) {                      /* <pre> - czcionka VGA */
            char buf[256];
            utf8_to_ascii(it->text, it->len, buf, sizeof(buf));
            gfx_text(x, y, buf, it->color);
        } else {
            font_draw(it->font, x, y, it->text, it->len, it->color);
        }
        if (it->underline) {
            int asc = it->font == FONT_MONO ? 12 : font_ascent(it->font);
            int ly = it->underline == 2 ? y + asc * 2 / 3 : y + asc + 2;   /* przekreslenie: przez srodek liter */
            gfx_fill(x - it->ul_left, ly, it->w + it->ul_left, 1, it->color);
        }
        break;
    case DL_IMAGE: {
        const struct image *im = it->img >= 0 && it->img < b->nimgs ? &b->imgs[it->img] : 0;
        if (!im || !im->px)
            break;
        if (im->w == it->w && im->h == it->h)
            gfx_blit(x, y, im->w, im->h, im->px);
        else
            gfx_blit_scaled(x, y, it->w, it->h, im->px, im->w, im->h);
        break;
    }
    case DL_CANVAS: {                                     /* <canvas>: piksele narysowane przez skrypt */
        const struct dom_canvas *cv = it->node ? it->node->canvas : 0;
        if (cv && cv->px)
            gfx_blit_alpha(x, y, it->w, it->h, cv->px, cv->w, cv->h);
        break;
    }
    }
}

/* ---- pasek adresu: tekst UTF-8, kursor skacze o cale znaki ---- */

static unsigned prev_char(const char *s, unsigned pos)
{
    while (pos > 0 && ((uint8_t)s[--pos] & 0xC0) == 0x80)
        ;
    return pos;
}

static unsigned next_char(const char *s, unsigned len, unsigned pos)
{
    if (pos < len)
        pos++;
    while (pos < len && ((uint8_t)s[pos] & 0xC0) == 0x80)
        pos++;
    return pos;
}

/* Od ktorego bajtu pokazywac adres, zeby kursor byl widoczny */
static unsigned addr_first(const struct browser *b, int visible_w)
{
    unsigned first = 0;
    if (b->addr_focus)
        while (first < b->cursor && font_width(FONT_REGULAR, b->addr + first, b->cursor - first) > visible_w)
            first = next_char(b->addr, b->addr_len, first);
    return first;
}

/* Uklad strony dla biezacej szerokosci okna (gdy sie zmienila albo strona sie zmienila) */
static void do_layout(struct window *w)
{
    struct browser *b = w->state;
    struct rect c = gui_client_size(w);
    struct rect v = view_rect(c);
    if (b->dom && b->layout_w != v.w) {
        layout_page(&b->lay, b->dom, v.w, b->imgs, b->nimgs);
        b->layout_w = v.w;
        b->content_h = b->lay.height;
    }
}

static void browser_draw(struct window *w, struct rect c)
{
    struct browser *b = w->state;
    struct rect rc = { 0, 0, c.w, c.h };

    /* Pasek narzedzi */
    gfx_gradient(c.x, c.y, c.w, TOOLBAR_H, 0xF3F4F6, 0xE5E7EB);
    gfx_fill(c.x, c.y + TOOLBAR_H - 1, c.w, 1, 0xD1D5DB);
    tool_button(c, back_rect(), "\x11", 1, b->nhist > 0);    /* strzalka w lewo */
    tool_button(c, home_rect(), "\x7F", 1, 1);               /* domek */
    tool_button(c, go_rect(rc), "Id\xC5\xBA", 0, 1);         /* "Idz" z kreska */
    struct rect a = addr_rect(rc);
    gfx_fill(c.x + a.x, c.y + a.y, a.w, a.h, 0xFFFFFF);
    gfx_frame(c.x + a.x, c.y + a.y, a.w, a.h, b->addr_focus ? gui_accent_color() : 0x9CA3AF);
    if (b->addr_focus)
        gfx_frame(c.x + a.x + 1, c.y + a.y + 1, a.w - 2, a.h - 2, gui_accent_color());
    {
        int vw = a.w - 16, ty = c.y + a.y + (a.h - font_height(FONT_REGULAR)) / 2;
        unsigned first = addr_first(b, vw);
        unsigned n = font_fit(FONT_REGULAR, b->addr + first, b->addr_len - first, vw);
        uint32_t color = COLOR_TEXT;
        if (!b->addr_focus && str_starts_ci(b->addr, "https://"))
            color = b->secure ? 0x047857 : 0xB45309;   /* zielony: nasz TLS, pomaranczowy: FrogFind */
        font_draw(FONT_REGULAR, c.x + a.x + 8, ty, b->addr + first, n, color);
        if (b->addr_focus && gui_is_focused(w)) {
            int cx = font_width(FONT_REGULAR, b->addr + first, b->cursor - first);
            gfx_fill(c.x + a.x + 8 + cx, c.y + a.y + 4, 2, a.h - 8, COLOR_TEXT);
        }
    }

    /* Strona */
    struct rect v = view_rect(rc);
    struct rect vs = { c.x + v.x, c.y + v.y, v.w, v.h };
    do_layout(w);
    gfx_fill(vs.x, vs.y, vs.w + SCROLLBAR_W, vs.h, b->dom ? b->lay.bg : 0xFFFFFF);
    int m = max_scroll(b, rc);
    if (b->scroll > m)
        b->scroll = m;
    struct rect saved = gfx_get_clip();
    struct rect clip = intersect(saved, vs);
    gfx_set_clip(&clip);
    for (unsigned i = 0; b->dom && i < b->lay.n; i++) {
        const struct dl_item *it = &b->lay.items[i];
        int sy = vs.y + it->y - b->scroll;
        if (sy + it->h < vs.y || sy > vs.y + vs.h)
            continue;
        paint_item(b, it, vs.x, vs.y - b->scroll);
    }
    gfx_set_clip(&saved);

    /* Pasek przewijania */
    int sbx = vs.x + vs.w;
    gfx_fill(sbx, vs.y, SCROLLBAR_W, vs.h, 0xF3F4F6);
    gfx_fill(sbx, vs.y, 1, vs.h, 0xE5E7EB);
    if (m > 0) {
        int thumb = vs.h * vs.h / b->content_h;
        if (thumb < 24)
            thumb = 24;
        int ty = vs.y + (vs.h - thumb) * b->scroll / m;
        gfx_fill(sbx + 3, ty, SCROLLBAR_W - 6, thumb, 0x9CA3AF);
    }

    /* Pasek stanu */
    int sy = c.y + c.h - STATUS_H;
    gfx_fill(c.x, sy, c.w, STATUS_H, 0xF3F4F6);
    gfx_fill(c.x, sy, c.w, 1, 0xD1D5DB);
    unsigned sl = (unsigned)strlen(b->status);
    unsigned n = font_fit(FONT_SMALL, b->status, sl, c.w - 16);
    font_draw(FONT_SMALL, c.x + 8, sy + (STATUS_H - font_height(FONT_SMALL)) / 2 + 1, b->status, n, b->status_color);

    /* Okienko alert() na srodku strony */
    if (b->alert[0]) {
        int aw = vs.w - 80 < 460 ? vs.w - 80 : 460, lines = 1, ah;
        char text[300];
        ksnprintf(text, sizeof(text), "%s", b->alert);
        for (char *q = text; *q; q++)
            if (*q == '\n')
                lines++;
        if (lines > 8)
            lines = 8;
        ah = 70 + lines * font_height(FONT_REGULAR);
        int ax = vs.x + (vs.w - aw) / 2, ay = vs.y + 60;
        gfx_fill(ax + 4, ay + 4, aw, ah, 0x9CA3AF);
        gfx_fill(ax, ay, aw, ah, 0xFFFFFF);
        gfx_frame(ax, ay, aw, ah, 0x6B7280);
        static const char head[] = "Strona m\xC3\xB3wi:";
        font_draw(FONT_BOLD, ax + 14, ay + 10, head, sizeof(head) - 1, COLOR_TEXT);
        char *line = text;
        for (int i = 0; i < lines && line; i++) {
            char *nl = line;
            while (*nl && *nl != '\n')
                nl++;
            int more = *nl == '\n';
            *nl = '\0';
            unsigned ll = (unsigned)strlen(line);
            font_draw(FONT_REGULAR, ax + 14, ay + 36 + i * font_height(FONT_REGULAR), line,
                      font_fit(FONT_REGULAR, line, ll, aw - 28), COLOR_TEXT);
            line = more ? nl + 1 : 0;
        }
        struct rect okb = { ax + aw - 74 - c.x, ay + ah - 34 - c.y, 60, 26 };
        tool_button(c, okb, "OK", 0, 1);
    }
}

/* ---------- ladowanie stron ---------- */

static void update_title(struct window *w)
{
    struct browser *b = w->state;
    char title[48];
    if (b->job)
        ksnprintf(title, sizeof(title), "Przegladarka - laduje...");
    else if (b->dom && b->dom->title[0]) {
        char ascii[96];
        utf8_to_ascii(b->dom->title, (unsigned)strlen(b->dom->title), ascii, sizeof(ascii));
        ksnprintf(title, sizeof(title), "%s - Przegladarka", ascii);
    }
    else
        ksnprintf(title, sizeof(title), "Przegladarka");
    gui_set_title(w, title);
}

static void imgjob_free(struct imgjob *j)
{
    for (int i = 0; i < j->n; i++) {
        kfree(j->urls[i]);
        image_free(&j->out[i]);
    }
    kfree(j->urls);
    kfree(j->out);
    kfree((void *)j->state);
    kfree(j);
}

static void img_thread(void *arg)
{
    struct imgjob *j = arg;
    for (int i = 0; i < j->n && !j->con->closed; i++) {
        const char *u = j->urls[i];
        uint8_t *data = 0;
        unsigned len = 0;
        struct http_response r;
        int from_http = 0;
        if (!u) {
            j->state[i] = 2;
            continue;
        }
        if (str_starts_ci(u, "data:")) {             /* obrazek wpisany w strone (base64) */
            const char *comma = u;
            while (*comma && *comma != ',')
                comma++;
            int b64 = 0;
            for (const char *q = u; q < comma; q++)
                if (str_starts_ci(q, ";base64"))
                    b64 = 1;
            if (*comma && b64)
                len = base64_decode(comma + 1, &data);
        } else if (http_get(u, &r, 0, 0) == 0) {
            if (r.status == 200) {
                data = (uint8_t *)r.body;
                len = r.body_len;
            }
            from_http = 1;
        }
        int ok = data && len && image_decode(data, len, &j->out[i], j->max_w, j->max_h) == 0;
        if (from_http)
            http_free(&r);
        else
            kfree(data);
        j->state[i] = ok ? 1 : 2;
    }
    j->done = 1;
    if (j->orphan)                                   /* okno juz nie czeka - sprzatamy sami */
        imgjob_free(j);
}

static void cancel_images(struct browser *b)
{
    struct imgjob *j = b->ij;
    if (!j)
        return;
    b->ij = 0;
    if (j->done) {
        console_destroy(j->con);
        imgjob_free(j);
    } else {
        j->con->closed = 1;                          /* przerwij pobieranie */
        j->orphan = 1;
    }
}

static void free_images(struct browser *b)
{
    cancel_images(b);
    for (int i = 0; i < b->nimgs; i++)
        image_free(&b->imgs[i]);
    kfree(b->imgs);
    b->imgs = 0;
    b->nimgs = b->imgs_done = b->imgs_total = b->imgs_requested = 0;
}

static void show_status(struct browser *b)
{
    if (b->imgs_total && b->imgs_done < b->imgs_total)
        ksnprintf(b->status, sizeof(b->status), "%s | obrazki %u/%u...", b->page_status, b->imgs_done, b->imgs_total);
    else
        ksnprintf(b->status, sizeof(b->status), "%s", b->page_status);
}

/* Obrazki do pobrania w tle: po zaladowaniu strony, a potem te dodane przez skrypt */
static void start_images(struct window *w)
{
    struct browser *b = w->state;
    const struct dom_doc *d = b->dom;
    if (!d || b->ij || strncmp_ci_url(b->url, "naszos:"))
        return;
    int first = b->imgs_requested, total = d->nimages < IMAGES_MAX ? d->nimages : IMAGES_MAX;
    if (total <= first)
        return;
    if (d->nimages > b->nimgs) {                         /* miejsce na nowe obrazki */
        struct image *ni = kzalloc(sizeof(struct image) * d->nimages);
        if (!ni)
            return;
        if (b->imgs)
            memcpy(ni, b->imgs, sizeof(struct image) * (unsigned)b->nimgs);
        kfree(b->imgs);
        b->imgs = ni;
        b->nimgs = (int)d->nimages;
    }
    b->imgs_requested = total;
    struct imgjob *j = kzalloc(sizeof(*j));
    if (!j)
        return;
    j->first = first;
    j->n = total - first;
    j->urls = kzalloc(sizeof(char *) * (unsigned)j->n);
    j->out = kzalloc(sizeof(struct image) * (unsigned)j->n);
    j->state = kzalloc((unsigned)j->n);
    struct rect c = gui_client_size(w);
    j->max_w = c.w - SCROLLBAR_W - 2 * PAD;
    if (j->max_w < 100)
        j->max_w = 100;
    j->max_h = 700;
    j->con = console_create(8, 2);
    if (!j->urls || !j->out || !j->state || !j->con) {
        if (j->con)
            console_destroy(j->con);
        j->n = j->urls ? j->n : 0;
        imgjob_free(j);
        return;
    }
    int wanted = 0;
    for (int i = 0; i < j->n; i++) {
        const char *src = d->images[first + i];
        char abs[URL_MAX];
        unsigned l = (unsigned)strlen(src);
        if (l > 4 && str_starts_ci(src + l - 4, ".svg"))
            continue;                                /* grafika wektorowa - nie umiemy */
        if (str_starts_ci(src, "data:")) {
            if (!str_starts_ci(src, "data:image/svg")) {
                j->urls[i] = kmalloc(l + 1);
                if (j->urls[i])
                    strcpy(j->urls[i], src);
            }
        } else if (url_resolve(b->url, src, abs, sizeof(abs))) {
            j->urls[i] = kmalloc(strlen(abs) + 1);
            if (j->urls[i])
                strcpy(j->urls[i], abs);
        }
        if (j->urls[i])
            wanted++;
    }
    if (!wanted || thread_create(img_thread, j, j->con) < 0) {
        console_destroy(j->con);
        imgjob_free(j);
        return;
    }
    b->ij = j;
    b->imgs_total = wanted;
    b->imgs_done = 0;
    show_status(b);
}

/* Z browser_tick: gotowe obrazki przechodza do okna */
static void poll_images(struct window *w)
{
    struct browser *b = w->state;
    struct imgjob *j = b->ij;
    if (!j)
        return;
    int changed = 0, finished = 0;
    struct dom_node *loaded[32];                     /* zdarzenia load/error dla skryptu strony */
    int8_t loaded_ok[32];
    int nloaded = 0;
    for (int i = 0; i < j->n; i++) {
        int idx = j->first + i;
        struct dom_node *node = b->dom && idx < b->dom->nimages ? b->dom->image_nodes[idx] : 0;
        if (j->state[i] == 1 && idx < b->nimgs) {
            image_free(&b->imgs[idx]);
            b->imgs[idx] = j->out[i];                /* przejmujemy piksele */
            j->out[i].px = 0;
            j->state[i] = 3;
            changed = 1;
            if (nloaded < 32) {
                loaded[nloaded] = node;
                loaded_ok[nloaded++] = 1;
            }
        } else if (j->state[i] == 2 && nloaded < 32) {
            j->state[i] = 4;                         /* blad - zglaszamy tylko raz */
            loaded[nloaded] = node;
            loaded_ok[nloaded++] = 0;
        }
        if (j->state[i] >= 2 || (j->state[i] == 0 && !j->urls[i]))
            finished++;
    }
    for (int i = 0; i < nloaded; i++)
        if (b->js && loaded[i])
            js_image_done(b->js, loaded[i], loaded_ok[i]);
    int counted = finished - (j->n - b->imgs_total);  /* bez pominietych (SVG itp.) */
    if (counted != b->imgs_done) {
        b->imgs_done = counted < 0 ? 0 : counted;
        changed = 1;
    }
    if (j->done) {
        b->ij = 0;
        console_destroy(j->con);
        imgjob_free(j);
        b->imgs_done = b->imgs_total;
        changed = 1;
    }
    if (changed) {
        show_status(b);
        b->layout_w = -1;                            /* obrazki maja swoj rozmiar - ukladamy od nowa */
        gui_redraw(w);
    }
}

static void js_attach(struct window *w);

static void set_doc(struct window *w, struct dom_doc *doc, struct js_rt *js, struct css_ctx *css,
                    const char *url, int push)
{
    struct browser *b = w->state;
    if (push && b->url[0]) {
        if (b->nhist == HIST_MAX) {                       /* najstarsza wypada */
            for (int i = 1; i < HIST_MAX; i++)
                memcpy(b->hist[i - 1], b->hist[i], URL_MAX);
            b->nhist--;
        }
        ksnprintf(b->hist[b->nhist++], URL_MAX, "%s", b->url);
    }
    free_images(b);
    js_free(b->js);                                       /* JS przed DOM - wskazuje na jego wezly */
    dom_free(b->dom);
    css_free(b->css);
    b->dom = doc;
    b->js = js;
    b->css = css;
    b->alert[0] = '\0';
    b->pending_nav[0] = '\0';
    b->pending_back = 0;
    js_attach(w);
    ksnprintf(b->url, sizeof(b->url), "%s", url);
    char shown[URL_MAX];
    display_url(url, shown, sizeof(shown));
    if (!b->addr_focus)
        set_addr(b, strcmp(url, START_URL) == 0 ? "" : shown);
    b->scroll = 0;
    b->layout_w = -1;
    update_title(w);
    gui_redraw(w);
}

static void job_progress(void *ctx, const char *msg)
{
    struct job *j = ctx;
    ksnprintf(j->status, sizeof(j->status), "%s", msg);
    j->status_changed = 1;
}

/* Style strony: arkusze z <link rel=stylesheet>, znaczniki <style>, i liczenie
 * wygladu kazdego elementu (css.c) */
static struct css_ctx *style_page(struct job *j, struct dom_doc *d, const char *base)
{
    struct css_ctx *c = css_new(j->view_w);
    if (!c)
        return 0;
    for (int i = 0; i < d->nsheets && (!j->con || !j->con->closed); i++) {
        char abs[URL_MAX], msg[80];
        if (!url_resolve(base, d->sheets[i], abs, sizeof(abs)))
            continue;
        if (j->con) {
            ksnprintf(msg, sizeof(msg), "Pobieram wyglad strony (CSS %d/%d)...", i + 1, d->nsheets);
            job_progress(j, msg);
        }
        struct http_response r;
        if (http_get(abs, &r, 0, 0) == 0) {
            if (r.status == 200)
                css_add(c, r.body, r.body_len);
            http_free(&r);
        }
    }
    if (d->css)
        css_add(c, d->css, d->css_len);
    if (j->con)
        job_progress(j, "Ukladam strone...");
    css_compute(c, d);
    return c;
}

/* ---------- JavaScript w trakcie ladowania (zadanie pobierajace) ---------- */

static void debug_log(const char *prefix, const char *text)
{
    /* port 0xE9 - QEMU zapisuje to do pliku (-debugcon), przydatne przy testach */
    for (const char *p = prefix; *p; p++)
        outb(0xE9, (uint8_t)*p);
    for (const char *p = text; *p; p++)
        outb(0xE9, (uint8_t)*p);
    outb(0xE9, '\n');
}

static void job_console(void *ctx, int level, const char *text)
{
    struct job *j = ctx;
    debug_log(level == 2 ? "[JS blad] " : level == 1 ? "[JS uwaga] " : "[JS] ", text);
    if (level == 2 && !j->js_error[0])
        ksnprintf(j->js_error, sizeof(j->js_error), "%s", text);
}

static void job_alert(void *ctx, const char *text)
{
    struct job *j = ctx;
    if (!j->alert[0])
        ksnprintf(j->alert, sizeof(j->alert), "%s", text);
}

static void job_navigate(void *ctx, const char *url)
{
    struct job *j = ctx;
    ksnprintf(j->nav, sizeof(j->nav), "%s", url);
}

/* Skrypt z src= (w trakcie ladowania strony) */
static int job_fetch(void *ctx, const char *url, char **body, unsigned *len)
{
    struct job *j = ctx;
    struct http_response r;
    char msg[160];
    ksnprintf(msg, sizeof(msg), "Pobieram skrypt %s...", url);
    job_progress(j, msg);
    if (http_get(url, &r, 0, 0) != 0)
        return -1;
    if (r.status != 200) {
        http_free(&r);
        return -1;
    }
    char *copy = kmalloc(r.body_len + 1);
    if (!copy) {
        http_free(&r);
        return -1;
    }
    memcpy(copy, r.body, r.body_len);
    copy[r.body_len] = '\0';
    *body = copy;
    *len = r.body_len;
    http_free(&r);
    return 0;
}

/* Parser natrafil na </script> - wykonujemy go od razu */
static void job_script(void *ctx, struct dom_doc *d, struct dom_node *script)
{
    struct job *j = ctx;
    if (j->con && j->con->closed)
        return;
    if (!j->js) {
        j->js = js_new(d, 0, j->resp.url[0] ? j->resp.url : j->url, &j->host);
        if (!j->js)
            return;
        if (j->con)
            js_set_cancel(j->js, &j->con->closed);
    }
    char msg[80], name[32];
    j->nscripts++;
    ksnprintf(msg, sizeof(msg), "JavaScript: skrypt %d...", j->nscripts);
    if (j->con)
        job_progress(j, msg);
    ksnprintf(name, sizeof(name), "skrypt %d", j->nscripts);
    js_run_script(j->js, script, name);
}

static void job_host(struct job *j)
{
    memset(&j->host, 0, sizeof(j->host));
    j->host.ctx = j;
    j->host.console = job_console;
    j->host.alert = job_alert;
    j->host.navigate = job_navigate;
    j->host.fetch = job_fetch;
    j->host.view_w = j->view_w;
    j->host.view_h = j->view_h;
}

/* Strona gotowa: DOMContentLoaded i load; po zmianach skryptow - style od nowa */
static void job_loaded(struct job *j)
{
    if (!j->js || !j->dom || !j->css || (j->con && j->con->closed))
        return;
    if (j->con)
        job_progress(j, "JavaScript: strona zaladowana (zdarzenie load)...");
    js_fire_loaded(j->js);
    if (js_dirty(j->js))
        css_compute(j->css, j->dom);
}

static void job_release(struct job *j)
{
    js_free(j->js);
    dom_free(j->dom);
    css_free(j->css);
    j->js = 0;
    j->dom = 0;
    j->css = 0;
}

static void job_thread(void *arg)
{
    struct job *j = arg;
    j->err = http_get(j->url, &j->resp, job_progress, j);
    if (!j->err) {
        int is_html = !j->resp.content_type[0] || str_starts_ci(j->resp.content_type, "text/html");
        job_host(j);
        j->dom = dom_parse_scripts(j->resp.body, j->resp.body_len, is_html,
                                   html_charset(j->resp.content_type, j->resp.body, j->resp.body_len),
                                   is_html ? job_script : 0, j);
        http_free(&j->resp);                              /* adres (resp.url) zostaje */
        if (!j->dom) {
            j->err = HTTP_ENOMEM;
        } else {
            j->css = style_page(j, j->dom, j->resp.url);
            job_loaded(j);
        }
    }
    j->done = 1;
    if (j->orphan) {                                      /* nikt juz nie czeka na wynik */
        job_release(j);
        kfree(j);                                         /* konsole zostawiamy - wlasnie jej uzywamy */
    }
}

/* Strona zapisana w przegladarce (startowa, blad, pokaz JS) - bez pobierania.
 * Skrypty wykonuja sie od razu, w petli pulpitu. */
static void local_page(struct window *w, const char *html, unsigned len, const char *url, int push)
{
    struct job *tmp = kzalloc(sizeof(*tmp));
    if (!tmp)
        return;
    struct rect c = gui_client_size(w);
    tmp->view_w = c.w - SCROLLBAR_W;
    tmp->view_h = c.h - TOOLBAR_H - STATUS_H;
    ksnprintf(tmp->url, sizeof(tmp->url), "%s", url);
    job_host(tmp);
    tmp->dom = dom_parse_scripts(html, len, 1, HC_UTF8, job_script, tmp);
    if (!tmp->dom) {
        job_release(tmp);
        kfree(tmp);
        return;
    }
    tmp->css = style_page(tmp, tmp->dom, url);
    job_loaded(tmp);
    struct browser *b = w->state;
    b->secure = 0;
    set_doc(w, tmp->dom, tmp->js, tmp->css, url, push);
    b->nscripts = tmp->nscripts;
    if (tmp->alert[0])
        ksnprintf(b->alert, sizeof(b->alert), "%s", tmp->alert);
    if (tmp->nav[0])
        ksnprintf(b->pending_nav, sizeof(b->pending_nav), "%s", tmp->nav);
    kfree(tmp);
}

/* Porzuca trwajace pobieranie (nowa strona albo zamkniete okno) */
static void cancel_job(struct browser *b)
{
    struct job *j = b->job;
    if (!j)
        return;
    b->job = 0;
    if (j->done) {
        job_release(j);
        console_destroy(j->con);
        kfree(j);
    } else {
        j->con->closed = 1;                               /* siec przerwie czekanie */
        j->orphan = 1;
    }
}

static void start_job(struct window *w, const char *url, int push)
{
    struct browser *b = w->state;
    cancel_job(b);
    if (strcmp(url, START_URL) == 0) {
        local_page(w, start_page, sizeof(start_page) - 1, START_URL, push);
        set_status(b, "Strona startowa NaszOS", COLOR_MUTED);
        gui_redraw(w);
        return;
    }
    if (strcmp(url, DEMO_URL) == 0) {
        local_page(w, demo_page, sizeof(demo_page) - 1, DEMO_URL, push);
        set_status(b, "Pokaz JavaScriptu - wszystko na tej stronie dziala dzieki JS", COLOR_MUTED);
        gui_redraw(w);
        return;
    }
    if (!net_info()->card) {
        set_status(b, "Brak karty sieciowej - uruchom NaszOS przez build.ps1 -Run", COLOR_ERROR);
        gui_redraw(w);
        return;
    }
    struct job *j = kzalloc(sizeof(*j));
    if (!j)
        return;
    ksnprintf(j->url, sizeof(j->url), "%s", url);
    j->push = push;
    j->view_w = gui_client_size(w).w - SCROLLBAR_W;
    j->view_h = gui_client_size(w).h - TOOLBAR_H - STATUS_H;
    j->con = console_create(8, 2);
    if (!j->con) {
        kfree(j);
        return;
    }
    if (thread_create(job_thread, j, j->con) < 0) {
        console_destroy(j->con);
        kfree(j);
        set_status(b, "Za duzo dzialajacych zadan - zamknij jakies okno", COLOR_ERROR);
        gui_redraw(w);
        return;
    }
    b->job = j;
    char shown[URL_MAX], msg[160];
    display_url(url, shown, sizeof(shown));
    if (str_starts_ci(url, PROXY_URL))
        ksnprintf(msg, sizeof(msg), "Szyfrowanie nie wyszlo - otwieram przez FrogFind (serwer posredniczacy)...");
    else
        ksnprintf(msg, sizeof(msg), "Otwieram %s...", shown);
    set_status(b, msg, COLOR_MUTED);
    update_title(w);
    gui_redraw(w);
}

static void proxy_url(const char *https, char *out, unsigned size)
{
    char enc[URL_MAX];
    url_encode(https, enc, sizeof(enc));
    ksnprintf(out, size, "%s%s", PROXY_URL, enc);
}

/* Adres z paska: strona, wyszukiwanie w Google albo strona startowa */
static void navigate(struct window *w, const char *input)
{
    while (*input == ' ')
        input++;
    char in[URL_MAX];
    ksnprintf(in, sizeof(in), "%s", input);
    unsigned n = (unsigned)strlen(in);
    while (n && in[n - 1] == ' ')
        in[--n] = '\0';
    if (!n || str_starts_ci(in, "naszos:")) {
        start_job(w, strcmp(in, DEMO_URL) == 0 ? DEMO_URL : START_URL, 1);
        return;
    }
    char url[URL_MAX];
    int has_dot = 0, has_space = 0;
    for (const char *s = in; *s; s++) {
        if (*s == '.') has_dot = 1;
        if (*s == ' ') has_space = 1;
    }
    if (str_starts_ci(in, "https://") || str_starts_ci(in, "http://")) {
        ksnprintf(url, sizeof(url), "%s", in);
    } else if (has_space || !has_dot) {                   /* to nie adres - szukamy */
        char enc[URL_MAX];
        url_encode(in, enc, sizeof(enc));
        ksnprintf(url, sizeof(url), "%s%s", SEARCH_URL, enc);
    } else {
        ksnprintf(url, sizeof(url), "http://%s", in);
    }
    start_job(w, url, 1);
}

/* Wartosc parametru (np. q) z adresu "...?q=wartosc&..." */
static int query_param(const char *url, const char *name, char *out, unsigned size)
{
    const char *q = url;
    while (*q && *q != '?')
        q++;
    unsigned nl = (unsigned)strlen(name);
    while (*q) {
        q++;
        if (memcmp(q, name, nl) == 0 && q[nl] == '=') {
            char raw[URL_MAX];
            unsigned n = 0;
            for (q += nl + 1; *q && *q != '&' && n + 1 < sizeof(raw); q++)
                raw[n++] = *q;
            raw[n] = '\0';
            url_decode(raw, out, size);
            return 1;
        }
        while (*q && *q != '&')
            q++;
    }
    return 0;
}

/* google.com, www.google.pl itp. */
static int strstr_host_google(const char *host)
{
    for (const char *s = host; *s; s++)
        if (str_starts_ci(s, "google."))
            return s == host || s[-1] == '.';
    return 0;
}

static void follow_link(struct window *w, int link)
{
    struct browser *b = w->state;
    if (!b->dom || link < 0 || (unsigned)link >= b->lay.nlinks)
        return;
    const char *href = b->lay.links[link];
    if (href[0] == '#') {                                 /* kotwica na tej samej stronie */
        b->scroll = 0;
        gui_redraw(w);
        return;
    }
    if (str_starts_ci(href, "javascript:")) {
        if (b->js) {
            js_run_url(b->js, href + 11);
            js_changes(w);
        }
        return;
    }
    if (str_starts_ci(href, "mailto:") || str_starts_ci(href, "tel:")) {
        set_status(b, "Tego linku NaszOS nie obsluzy (e-mail / telefon)", COLOR_ERROR);
        gui_redraw(w);
        return;
    }
    char abs[URL_MAX];
    if (str_starts_ci(href, "naszos:"))
        ksnprintf(abs, sizeof(abs), "%s", href);
    else if (str_starts_ci(b->url, "naszos:"))
        ksnprintf(abs, sizeof(abs), "%s", href);
    else if (!url_resolve(b->url, href, abs, sizeof(abs)))
        return;
    /* Wyniki Google prowadza przez "/url?q=prawdziwy-adres" - idziemy od razu tam */
    struct url u;
    char target[URL_MAX];
    if (url_parse(abs, &u) && strstr_host_google(u.host) && str_starts_ci(u.path, "/url?") &&
        (query_param(abs, "q", target, sizeof(target)) || query_param(abs, "url", target, sizeof(target))) &&
        (str_starts_ci(target, "http://") || str_starts_ci(target, "https://")))
        ksnprintf(abs, sizeof(abs), "%s", target);
    start_job(w, abs, 1);
}

static void go_back(struct window *w)
{
    struct browser *b = w->state;
    if (b->nhist == 0)
        return;
    char url[URL_MAX];
    ksnprintf(url, sizeof(url), "%s", b->hist[--b->nhist]);
    start_job(w, url, 0);
}

static void finish_job(struct window *w)
{
    struct browser *b = w->state;
    struct job *j = b->job;
    b->job = 0;
    if (j->err && j->resp.https_url[0] && !str_starts_ci(j->url, PROXY_URL)) {
        /* Nasz TLS nie dal rady (np. serwer zna tylko TLS 1.2) - plan B: FrogFind */
        char via[URL_MAX];
        proxy_url(j->resp.https_url, via, sizeof(via));
        int push = j->push;
        console_destroy(j->con);
        kfree(j);
        start_job(w, via, push);
        return;
    }
    if (j->err) {
        char page[URL_MAX + 400], shown[URL_MAX];
        display_url(j->url, shown, sizeof(shown));
        int len = ksnprintf(page, sizeof(page),
                            "<title>Blad</title><h1>Nie udalo sie otworzyc strony</h1>"
                            "<p>Adres: <b>%s</b></p><p>Powod: %s</p>"
                            "<p>Sprawdz adres albo sprobuj pozniej. <a href=\"" START_URL "\">Strona startowa</a></p>",
                            shown, http_strerror(j->err));
        local_page(w, page, (unsigned)len, j->url, j->push);
        set_status(b, http_strerror(j->err), COLOR_ERROR);
    } else {
        unsigned status = j->resp.status;
        char msg[160];
        b->secure = j->resp.secure;
        if (!j->dom->title[0]) {                     /* np. Wikipedia w trybie "sam artykul" */
            char shown[URL_MAX], name[96];
            display_url(j->resp.url, shown, sizeof(shown));
            const char *last = shown;
            for (const char *q = shown; *q; q++)
                if (*q == '/')
                    last = q + 1;
            url_decode(last, name, sizeof(name));
            for (char *q = name; *q; q++)
                if (*q == '_')
                    *q = ' ';
            ksnprintf(j->dom->title, sizeof(j->dom->title), "%s", name);
        }
        set_doc(w, j->dom, j->js, j->css, j->resp.url, j->push);
        b->nscripts = j->nscripts;
        j->dom = 0;
        j->js = 0;
        j->css = 0;
        if (j->alert[0])
            ksnprintf(b->alert, sizeof(b->alert), "%s", j->alert);
        if (j->nav[0])
            ksnprintf(b->pending_nav, sizeof(b->pending_nav), "%s", j->nav);
        if (status == 200 && j->resp.secure)
            ksnprintf(msg, sizeof(msg), "Gotowe - szyfrowane: %s (certyfikat niesprawdzony)", j->resp.cipher);
        else if (status == 200 && str_starts_ci(j->url, PROXY_URL))
            ksnprintf(msg, sizeof(msg), "Gotowe - przez FrogFind (serwer posredniczacy widzi te strone)");
        else if (status == 200)
            ksnprintf(msg, sizeof(msg), "Gotowe (bez szyfrowania)");
        else
            ksnprintf(msg, sizeof(msg), "Serwer odpowiedzial: %u%s", status,
                      status == 404 ? " - nie ma takiej strony" : "");
        if (b->nscripts) {
            unsigned l = (unsigned)strlen(msg);
            if (j->js_error[0])
                ksnprintf(msg + l, sizeof(msg) - l, " | JS: blad - %s", j->js_error);
            else
                ksnprintf(msg + l, sizeof(msg) - l, " | JavaScript: %d %s", b->nscripts,
                          b->nscripts == 1 ? "skrypt" : b->nscripts < 5 ? "skrypty" : "skryptow");
        }
        set_status(b, msg, status == 200 && !j->js_error[0] ? COLOR_MUTED : COLOR_ERROR);
        start_images(w);
    }
    console_destroy(j->con);
    kfree(j);
    update_title(w);
    gui_redraw(w);
}

/* ---------- JavaScript w oknie ---------- */

/* Skrypt zmienil strone: style od nowa, nowy uklad, ewentualnie nowe obrazki */
static void js_changes(struct window *w)
{
    struct browser *b = w->state;
    if (!b->js || !b->dom)
        return;
    if (js_dirty(b->js)) {
        if (b->css)
            css_compute(b->css, b->dom);
        b->layout_w = -1;
        update_title(w);
        start_images(w);
        gui_redraw(w);
    }
    if (b->dom->canvas_gen != b->canvas_gen) {          /* skrypt narysowal cos na <canvas> */
        b->canvas_gen = b->dom->canvas_gen;
        gui_redraw(w);
    }
}

static void b_console(void *ctx, int level, const char *text)
{
    struct window *w = ctx;
    struct browser *b = w->state;
    debug_log(level == 2 ? "[JS blad] " : level == 1 ? "[JS uwaga] " : "[JS] ", text);
    /* ostatni komunikat konsoli na pasku stanu (blad nie znika pod zwyklym logiem) */
    if (level == 2 || b->status_color != COLOR_ERROR) {
        ksnprintf(b->status, sizeof(b->status), "%s%s", level == 2 ? "Blad JavaScript: " : "Konsola JS: ", text);
        b->status_color = level == 2 ? COLOR_ERROR : level == 1 ? 0xB45309 : COLOR_MUTED;
        gui_redraw(w);
    }
}

static void b_alert(void *ctx, const char *text)
{
    struct window *w = ctx;
    struct browser *b = w->state;
    ksnprintf(b->alert, sizeof(b->alert), "%s", text[0] ? text : " ");
    gui_redraw(w);
}

static void b_navigate(void *ctx, const char *url)
{
    struct window *w = ctx;
    struct browser *b = w->state;
    ksnprintf(b->pending_nav, sizeof(b->pending_nav), "%s", url);
}

static void b_back(void *ctx)
{
    struct window *w = ctx;
    struct browser *b = w->state;
    b->pending_back = 1;
}

static void b_scroll_to(void *ctx, int y)
{
    struct window *w = ctx;
    struct browser *b = w->state;
    do_layout(w);
    int m = max_scroll(b, gui_client_size(w));
    b->scroll = y < 0 ? 0 : y > m ? m : y;
    gui_redraw(w);
}

static int b_scroll_y(void *ctx)
{
    struct window *w = ctx;
    struct browser *b = w->state;
    return b->scroll;
}

static int is_inside(const struct dom_node *n, const struct dom_node *anc)
{
    for (int depth = 0; n && depth < 200; n = n->parent, depth++)
        if (n == anc)
            return 1;
    return 0;
}

/* Prostokat elementu na stronie: suma wszystkiego, co z niego narysowano */
static int b_box(void *ctx, struct dom_node *n, int *x, int *y, int *wd, int *ht)
{
    struct window *w = ctx;
    struct browser *b = w->state;
    if (!b->dom)
        return 0;
    if (js_dirty(b->js)) {                               /* uklad musi byc aktualny */
        if (b->css)
            css_compute(b->css, b->dom);
        b->layout_w = -1;
        gui_redraw(w);
    }
    do_layout(w);
    int x1 = 1 << 30, y1 = 1 << 30, x2 = -(1 << 30), y2 = -(1 << 30);
    for (unsigned i = 0; i < b->lay.n; i++) {
        const struct dl_item *it = &b->lay.items[i];
        if (!it->node || !is_inside(it->node, n) || it->w <= 0 || it->h <= 0)
            continue;
        if (it->x < x1) x1 = it->x;
        if (it->y < y1) y1 = it->y;
        if (it->x + it->w > x2) x2 = it->x + it->w;
        if (it->y + it->h > y2) y2 = it->y + it->h;
    }
    if (x2 < x1)
        return 0;
    *x = x1;
    *y = y1;
    *wd = x2 - x1;
    *ht = y2 - y1;
    return 1;
}

/* Piksele pobranego obrazka - dla ctx.drawImage(img, ...) */
static int b_image(void *ctx, struct dom_node *n, int *iw, int *ih, const uint32_t **px)
{
    struct window *w = ctx;
    struct browser *b = w->state;
    if (!n || n->img < 0 || n->img >= b->nimgs || !b->imgs[n->img].px)
        return 0;
    *iw = b->imgs[n->img].w;
    *ih = b->imgs[n->img].h;
    *px = b->imgs[n->img].px;
    return 1;
}

/* JavaScript strony przechodzi pod opieke okna (po zaladowaniu) */
static void js_attach(struct window *w)
{
    struct browser *b = w->state;
    if (!b->js)
        return;
    struct rect c = gui_client_size(w);
    struct js_host h;
    memset(&h, 0, sizeof(h));
    h.ctx = w;
    h.console = b_console;
    h.alert = b_alert;
    h.navigate = b_navigate;
    h.back = b_back;
    h.scroll_to = b_scroll_to;
    h.scroll_y = b_scroll_y;
    h.box = b_box;
    h.image = b_image;
    h.view_w = c.w - SCROLLBAR_W;
    h.view_h = c.h - TOOLBAR_H - STATUS_H;
    js_set_host(b->js, &h);
    js_set_cancel(b->js, 0);
}

/* ---------- obsluga okna ---------- */

static void browser_tick(struct window *w)
{
    struct browser *b = w->state;
    poll_images(w);
    if (b->js && !b->alert[0]) {
        js_timers(b->js);                                /* setTimeout, setInterval, animacje */
        js_changes(w);
    }
    if (b->pending_nav[0] && !b->alert[0]) {
        char url[URL_MAX];
        ksnprintf(url, sizeof(url), "%s", b->pending_nav);
        b->pending_nav[0] = '\0';
        start_job(w, url, 1);
    }
    if (b->pending_back) {
        b->pending_back = 0;
        go_back(w);
    }
    if (!b->ij && b->dom && b->imgs_requested < b->dom->nimages && b->imgs_requested < IMAGES_MAX)
        start_images(w);
    struct job *j = b->job;
    if (!j)
        return;
    if (j->status_changed) {
        j->status_changed = 0;
        set_status(b, j->status, COLOR_MUTED);
        gui_redraw(w);
    }
    if (j->done)
        finish_job(w);
}

static void browser_click(struct window *w, int x, int y, int dbl)
{
    (void)dbl;
    struct browser *b = w->state;
    struct rect c = gui_client_size(w);
    struct rect bk = back_rect(), hm = home_rect(), go = go_rect(c), a = addr_rect(c), v = view_rect(c);
    int was_focus = b->addr_focus;
    if (b->alert[0]) {                                    /* okienko alert: klik zamyka */
        b->alert[0] = '\0';
        gui_redraw(w);
        return;
    }
    if (!rect_contains(&a, x, y))
        b->addr_focus = 0;

    if (rect_contains(&bk, x, y)) {
        go_back(w);
    } else if (rect_contains(&hm, x, y)) {
        start_job(w, START_URL, 1);
    } else if (rect_contains(&go, x, y)) {
        navigate(w, b->addr);
    } else if (rect_contains(&a, x, y)) {
        if (!was_focus) {                                 /* pierwsze klikniecie zaznacza calosc */
            b->addr_focus = 1;
            b->cursor = b->addr_len;
        } else {
            /* Kursor tam, gdzie kliknieto - litery maja rozne szerokosci */
            unsigned first = addr_first(b, a.w - 16), pos = first;
            int px = a.x + 8;
            while (pos < b->addr_len) {
                unsigned next = next_char(b->addr, b->addr_len, pos);
                int cw = font_width(FONT_REGULAR, b->addr + pos, next - pos);
                if (px + cw / 2 > x)
                    break;
                px += cw;
                pos = next;
            }
            b->cursor = pos;
        }
    } else if (x >= v.x + v.w && y >= v.y && y < v.y + v.h) {    /* pasek przewijania */
        int m = max_scroll(b, c);
        if (m > 0) {
            int thumb_y = v.y + (v.h - 24) * b->scroll / m;
            scroll_by(w, y < thumb_y ? -(v.h - 40) : v.h - 40);
        }
    } else if (rect_contains(&v, x, y) && b->dom) {
        int px = x - v.x, py = y - v.y + b->scroll;
        int link = -1;
        struct dom_node *target = 0;
        for (int i = (int)b->lay.n - 1; i >= 0; i--) {    /* od narysowanych na wierzchu */
            const struct dl_item *it = &b->lay.items[i];
            if (!target && it->node && px >= it->x && px < it->x + it->w && py >= it->y && py < it->y + it->h)
                target = it->node;
            if (link < 0 && it->link >= 0 && px >= it->x - 2 && px < it->x + it->w + 2 &&
                py >= it->y - 2 && py < it->y + it->h + 2)
                link = it->link;
            if (target && link >= 0)
                break;
        }
        int prevented = 0;
        if (b->js) {                                      /* najpierw skrypt strony (onclick) */
            prevented = js_click(b->js, target, px, py);
            js_changes(w);
        }
        if (!prevented && link >= 0 && !b->pending_nav[0]) {
            follow_link(w, link);
            return;
        }
    }
    gui_redraw(w);
}

static void browser_key(struct window *w, int key)
{
    struct browser *b = w->state;
    struct rect c = gui_client_size(w);
    int page = view_rect(c).h - 40;

    if (b->alert[0]) {                                    /* okienko alert: Enter/Esc/spacja zamyka */
        if (key == '\n' || key == 27 || key == ' ') {
            b->alert[0] = '\0';
            gui_redraw(w);
        }
        return;
    }
    int page_keys = 0;
    if (!b->addr_focus && b->js) {
        /* strona z obsluga klawiszy (gra) albo kursor w polu tekstowym strony */
        page_keys = js_wants_keys(b->js) || js_input_focused(b->js);
        if (page_keys || key >= KEY_UP || key == '\b') {
            int handled = js_key(b->js, key);
            js_changes(w);
            if (handled)
                return;
        }
    }
    if (!b->addr_focus && !page_keys && key > ' ' && key < 127) {   /* pisanie = nowy adres */
        b->addr_focus = 1;
        b->addr[0] = '\0';
        b->addr_len = b->cursor = 0;
    }
    if (b->addr_focus) {
        switch (key) {
        case '\n':
            b->addr_focus = 0;
            navigate(w, b->addr);
            return;
        case 27: {                                        /* Esc - przywroc adres strony */
            char shown[URL_MAX];
            display_url(b->url, shown, sizeof(shown));
            set_addr(b, strcmp(b->url, START_URL) == 0 ? "" : shown);
            b->addr_focus = 0;
            break;
        }
        case '\b':
            if (b->cursor > 0) {
                unsigned from = prev_char(b->addr, b->cursor);
                memmove(b->addr + from, b->addr + b->cursor, b->addr_len - b->cursor + 1);
                b->addr_len -= b->cursor - from;
                b->cursor = from;
            }
            break;
        case KEY_DELETE:
            if (b->cursor < b->addr_len) {
                unsigned to = next_char(b->addr, b->addr_len, b->cursor);
                memmove(b->addr + b->cursor, b->addr + to, b->addr_len - to + 1);
                b->addr_len -= to - b->cursor;
            }
            break;
        case KEY_LEFT:  b->cursor = prev_char(b->addr, b->cursor); break;
        case KEY_RIGHT: b->cursor = next_char(b->addr, b->addr_len, b->cursor); break;
        case KEY_HOME:  b->cursor = 0; break;
        case KEY_END:   b->cursor = b->addr_len; break;
        default:
            if (key >= ' ' && key < 127 && b->addr_len + 1 < sizeof(b->addr)) {
                memmove(b->addr + b->cursor + 1, b->addr + b->cursor, b->addr_len - b->cursor + 1);
                b->addr[b->cursor++] = (char)key;
                b->addr_len++;
            }
        }
        gui_redraw(w);
        return;
    }
    switch (key) {
    case KEY_UP:    scroll_by(w, -40); break;
    case KEY_DOWN:  scroll_by(w, 40); break;
    case KEY_PGUP:  scroll_by(w, -page); break;
    case KEY_PGDN:
    case ' ':       scroll_by(w, page); break;
    case KEY_HOME:  scroll_by(w, -b->content_h); break;
    case KEY_END:   scroll_by(w, b->content_h); break;
    case '\b':      go_back(w); break;
    case '\n':
        b->addr_focus = 1;
        b->cursor = b->addr_len;
        gui_redraw(w);
        break;
    }
}

static void browser_scroll(struct window *w, int delta)
{
    scroll_by(w, delta * 3 * LINE_H);
}

static void browser_resize(struct window *w)
{
    struct browser *b = w->state;
    b->layout_w = -1;
    if (b->js) {
        struct rect c = gui_client_size(w);
        js_resize(b->js, c.w - SCROLLBAR_W, c.h - TOOLBAR_H - STATUS_H);
        js_changes(w);
    }
    gui_redraw(w);
}

static void browser_destroy(struct window *w)
{
    struct browser *b = w->state;
    cancel_job(b);
    free_images(b);
    js_free(b->js);
    dom_free(b->dom);
    css_free(b->css);
    layout_free(&b->lay);
    kfree(b);
}

static const struct app browser_app = {
    .draw = browser_draw, .click = browser_click, .key = browser_key, .tick = browser_tick,
    .scroll = browser_scroll, .resize = browser_resize, .destroy = browser_destroy,
    .wants_escape = 1, .min_w = 420, .min_h = 240,
};

void app_open_browser(void)
{
    struct browser *b = kzalloc(sizeof(*b));
    if (!b)
        return;
    b->layout_w = -1;
    b->status_color = COLOR_MUTED;
    int cw = gfx_width() - 200, ch = gfx_height() - 140;
    if (cw > 1000) cw = 1000;
    if (ch > 720) ch = 720;
    struct window *w = gui_open(&browser_app, "Przegladarka", cw, ch, b);
    if (!w) {
        kfree(b);
        return;
    }
    gui_maximize(w);                                 /* strony czyta sie najlepiej na calym ekranie */
    start_job(w, START_URL, 0);
}

/* Komendy sieciowe: siec, ping, dns, www, czas.
 *
 * www pobiera strone (src/http.c), zamienia HTML na tekst (src/html.c)
 * i pokazuje go ekran po ekranie. Strony szyfrowane (https://) wymagaja
 * TLS - tego NaszOS jeszcze nie umie. */

#include <stdint.h>
#include "shell_net.h"
#include "net.h"
#include "vga.h"
#include "heap.h"
#include "fs.h"
#include "string.h"
#include "keyboard.h"
#include "timer.h"
#include "rtc.h"
#include "http.h"
#include "html.h"
#include "utf8.h"

#define SAVE_PATH  "/strona.txt"

static void red(const char *s)
{
    vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    vga_write(s);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static char *next_word(char **args)
{
    char *s = *args;
    while (*s == ' ')
        s++;
    if (!*s)
        return 0;
    char *w = s;
    while (*s && *s != ' ')
        s++;
    if (*s)
        *s++ = '\0';
    *args = s;
    return w;
}

static int require_net(void)
{
    const struct net_info *n = net_info();
    if (!n->card) {
        red("Brak karty sieciowej. Uruchom NaszOS przez build.ps1 -Run (QEMU z karta RTL8139).\n");
        return 0;
    }
    return 1;
}

/* ---------- siec ---------- */

void cmd_siec(char *args)
{
    (void)args;
    if (!require_net())
        return;
    const struct net_info *n = net_info();
    char ip[16], mask[16], gw[16], dns[16];
    net_format_ip(n->ip, ip);
    net_format_ip(n->mask, mask);
    net_format_ip(n->gateway, gw);
    net_format_ip(n->dns, dns);
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_write("Karta sieciowa Realtek RTL8139\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("  Adres MAC:  %02x:%02x:%02x:%02x:%02x:%02x   (numer seryjny karty)\n",
            n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
    kprintf("  Adres IP:   %s   (%s)\n", ip, n->dhcp ? "z DHCP" : "ustawiony recznie");
    kprintf("  Maska:      %s\n", mask);
    kprintf("  Brama:      %s   (router - przez niego idzie ruch do internetu)\n", gw);
    kprintf("  DNS:        %s   (zamienia nazwy na adresy IP)\n", dns);
    kprintf("  Ramki:      odebrane %u, wyslane %u\n", n->rx_frames, n->tx_frames);
}

/* ---------- dns ---------- */

void cmd_dns(char *args)
{
    char *name = next_word(&args);
    if (!name) {
        vga_write("Uzycie: dns <nazwa>, np. dns google.com\n");
        return;
    }
    if (!require_net())
        return;
    net_lock();
    uint32_t ip;
    int err = net_resolve(name, &ip);
    net_unlock();
    if (err) {
        red(name);
        red(": ");
        red(net_strerror(err));
        red("\n");
        return;
    }
    char buf[16];
    net_format_ip(ip, buf);
    kprintf("%s ma adres %s\n", name, buf);
}

/* ---------- ping ---------- */

void cmd_ping(char *args)
{
    char *name = next_word(&args);
    if (!name) {
        vga_write("Uzycie: ping <adres> [ile], np. ping google.com albo ping 10.0.2.2\n");
        return;
    }
    char *count_s = next_word(&args);
    unsigned count = 4;
    if (count_s) {
        count = 0;
        for (; *count_s >= '0' && *count_s <= '9'; count_s++)
            count = count * 10 + (unsigned)(*count_s - '0');
        if (count < 1 || count > 100)
            count = 4;
    }
    if (!require_net())
        return;

    net_lock();
    uint32_t ip;
    int err = net_resolve(name, &ip);
    if (err) {
        net_unlock();
        red(name);
        red(": ");
        red(net_strerror(err));
        red("\n");
        return;
    }
    char buf[16];
    net_format_ip(ip, buf);
    kprintf("Ping %s [%s]:\n", name, buf);

    unsigned got = 0;
    uint32_t min = 0xFFFFFFFFu, max = 0, total = 0;
    for (unsigned i = 0; i < count && !con_cur->closed; i++) {
        uint32_t started = timer_uptime_ms();
        uint32_t rtt;
        unsigned ttl;
        err = net_ping(ip, (uint16_t)(i + 1), 2000, &rtt, &ttl);
        if (err == NET_EABORT)
            break;
        if (err) {
            red("  Brak odpowiedzi\n");
        } else {
            got++;
            total += rtt;
            if (rtt < min) min = rtt;
            if (rtt > max) max = rtt;
            kprintf("  Odpowiedz od %s: nr=%u czas=%u.%02u ms TTL=%u\n",
                    buf, i + 1, rtt / 1000, rtt % 1000 / 10, ttl);
        }
        /* Odstep ok. 1 s miedzy pingami (jak w Windowsie) */
        while (i + 1 < count && timer_uptime_ms() - started < 1000 && !con_cur->closed)
            timer_sleep(10);
    }
    net_unlock();
    kprintf("Wyslano %u, odebrano %u, zgubiono %u", count, got, count - got);
    if (got)
        kprintf(" | czas: min %u.%02u, max %u.%02u, srednio %u.%02u ms",
                min / 1000, min % 1000 / 10, max / 1000, max % 1000 / 10,
                total / got / 1000, total / got % 1000 / 10);
    vga_putc('\n');
    if (!got && err == NET_ETIMEOUT)
        vga_write("(Uwaga: niektore sieci blokuja ping. Sprobuj: ping 10.0.2.2 albo www example.com)\n");
}

/* ---------- www: strona w terminalu ---------- */

/* Wyswietla tekst z zawijaniem slow i zatrzymaniem co ekran */
static void show_paged(const char *text)
{
    unsigned width = VGA_WIDTH - 1, lines = 0, page = VGA_HEIGHT > 4 ? VGA_HEIGHT - 3 : 10;
    const char *s = text;
    while (*s && !con_cur->closed) {
        /* Jedna linia ekranu: do \n albo do ostatniej spacji przed szerokoscia */
        unsigned n = 0, brk = 0;
        while (s[n] && s[n] != '\n' && n < width) {
            if (s[n] == ' ')
                brk = n;
            n++;
        }
        if (s[n] && s[n] != '\n' && brk)
            n = brk;
        for (unsigned i = 0; i < n; i++)
            vga_putc(s[i]);
        vga_putc('\n');
        s += n;
        if (*s == '\n' || *s == ' ')
            s++;
        if (++lines >= page && *s) {
            vga_set_color(VGA_BLACK, VGA_LIGHT_GREY);
            vga_write(" -- Spacja/Enter: dalej, Q: koniec -- ");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            int k;
            do
                k = keyboard_getkey();
            while (k != ' ' && k != '\n' && k != 'q' && k != 'Q' && k != 27 && !con_cur->closed);
            unsigned x, y;
            vga_get_pos(&x, &y);
            vga_set_pos(0, y);
            vga_fill_row(y, VGA_LIGHT_GREY, VGA_BLACK);
            if (k == 'q' || k == 'Q' || k == 27)
                return;
            lines = k == '\n' ? page - 1 : 0;                  /* Enter: jedna linia dalej */
        }
    }
}

static void www_progress(void *ctx, const char *msg)
{
    (void)ctx;
    vga_set_color(VGA_DARK_GREY, VGA_BLACK);
    vga_write(msg);
    vga_putc('\n');
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

void cmd_www(char *args)
{
    char *addr = next_word(&args);
    if (!addr) {
        vga_write("Uzycie: www <adres>, np. www example.com albo www info.cern.ch\n"
                  "(Na pulpicie jest tez Przegladarka - z klikanymi linkami.)\n");
        return;
    }
    if (!require_net())
        return;

    struct http_response r;
    int err = http_get(addr, &r, www_progress, 0);
    if (err) {
        red("Blad: ");
        red(http_strerror(err));
        red("\n");
        if (err == HTTP_EHTTPS)
            red("Sprobuj stron z http://, np. www example.com - albo Przegladarki na pulpicie,\n"
                "ktora otwiera strony https przez serwer posredniczacy FrogFind.\n");
        return;
    }
    kprintf("Odpowiedz serwera: %u %s\n", r.status,
            r.status == 200 ? "(OK)" : r.status == 404 ? "(nie ma takiej strony)" : "");
    if (r.secure) {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        kprintf("Szyfrowane: %s (certyfikat niesprawdzony)\n", r.cipher);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }

    int is_html = !r.content_type[0] || str_starts_ci(r.content_type, "text/html");
    struct html_doc *doc = html_parse(r.body, r.body_len, is_html,
                                      html_charset(r.content_type, r.body, r.body_len));
    http_free(&r);
    unsigned len = 0;
    char *text = doc ? html_to_plain(doc, &len) : 0;
    if (!text) {
        html_free(doc);
        red("Za malo pamieci.\n");
        return;
    }

    /* Zapisujemy tekst - mozna go potem otworzyc w Notatniku */
    struct fs_node *f;
    int saved = 0;
    if (fs_lookup(fs_root(), SAVE_PATH, &f) == FS_OK ||
        fs_create(fs_root(), SAVE_PATH, 0, &f) == FS_OK)
        saved = !f->is_dir && fs_write(f, text, len, 0) == FS_OK;

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    char title[96];                          /* tytul w UTF-8 -> bez ogonkow dla czcionki VGA */
    utf8_to_ascii(doc->title, (unsigned)strlen(doc->title), title, sizeof(title));
    kprintf("--- %s (%u znakow tekstu) ---\n", title[0] ? title : r.url, len);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    show_paged(text);
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    if (saved)
        vga_write("--- koniec. Tekst zapisany w " SAVE_PATH " ---\n");
    else
        vga_write("--- koniec ---\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kfree(text);
    html_free(doc);
}

/* ---------- czas z internetu ---------- */

#define NTP_SERVER "pool.ntp.org"

int net_clock_sync(int64_t *diff_secs, struct rtc_time *now)
{
    net_lock();
    int64_t utc;
    int err = net_ntp(NTP_SERVER, &utc);
    net_unlock();
    if (err)
        return err;
    int64_t local = utc + rtc_poland_offset(utc);   /* zegar komputera chodzi w czasie polskim */
    struct rtc_time before;
    rtc_read(&before);
    *diff_secs = rtc_to_unix(&before) - local;
    /* +-1 s to tylko zaokraglenie (ulamki sekundy) - wtedy nie ruszamy zegara */
    if (*diff_secs >= -1 && *diff_secs <= 1)
        *diff_secs = 0;
    rtc_from_unix(local, now);
    if (*diff_secs != 0)
        rtc_write(now);
    return 0;
}

void cmd_czas(char *args)
{
    (void)args;
    if (!require_net())
        return;
    kprintf("Pytam serwer czasu %s...\n", NTP_SERVER);
    int64_t diff;
    struct rtc_time t;
    int err = net_clock_sync(&diff, &t);
    if (err) {
        red("Blad: ");
        red(net_strerror(err));
        red("\n");
        return;
    }
    int64_t utc = rtc_to_unix(&t);
    int summer = rtc_poland_offset(utc - 3600) == 7200;
    kprintf("Czas z internetu: %s, %u %s %u, %02u:%02u:%02u (czas polski %s, UTC+%u)\n",
            rtc_day_name[t.weekday], t.day, rtc_month_genitive[t.month - 1], t.year,
            t.hour, t.minute, t.second, summer ? "letni" : "zimowy", summer ? 2 : 1);
    if (diff == 0)
        vga_write("Zegar komputera jest dokladny co do sekundy.\n");
    else
        kprintf("Zegar komputera %s o %ld s - poprawiony.\n",
                diff > 0 ? "spieszyl sie" : "spoznial sie", diff > 0 ? diff : -diff);
}

/* ---------- pobierz: dowolny plik z internetu ---------- */

void cmd_pobierz(char *args)
{
    char *addr = next_word(&args);
    char *name = next_word(&args);
    if (!addr) {
        vga_write("Uzycie: pobierz <adres> [plik], np. pobierz example.com/plik.wav muzyka.wav\n");
        return;
    }
    if (!require_net())
        return;
    char fname[64];
    if (name) {
        ksnprintf(fname, sizeof(fname), "%s", name);
    } else {                                   /* nazwa z adresu: ostatni czlon sciezki */
        struct url u;
        const char *last = "pobrane";
        if (url_parse(addr, &u)) {
            for (const char *s = u.path; *s && *s != '?'; s++)
                if (*s == '/' && s[1] && s[1] != '?')
                    last = s + 1;
        }
        unsigned n = 0;
        for (; last[n] && last[n] != '?' && last[n] != '/' && n + 1 < sizeof(fname); n++)
            fname[n] = last[n];
        fname[n] = '\0';
        if (!n)
            ksnprintf(fname, sizeof(fname), "pobrane");
    }

    struct http_response r;
    int err = http_get(addr, &r, www_progress, 0);
    if (err) {
        red("Blad: ");
        red(http_strerror(err));
        red("\n");
        return;
    }
    if (r.status != 200) {
        kprintf("Serwer odpowiedzial %u - nic nie zapisano.\n", r.status);
        http_free(&r);
        return;
    }
    struct fs_node *cwd = fs_node_valid(con_cur->cwd) ? con_cur->cwd : fs_root();
    struct fs_node *f;
    int ferr = fs_lookup(cwd, fname, &f);
    if (ferr == FS_ENOENT)
        ferr = fs_create(cwd, fname, 0, &f);
    if (ferr == FS_OK && !f->is_dir)
        ferr = fs_write(f, r.body, r.body_len, 0);
    else if (ferr == FS_OK)
        ferr = FS_EISDIR;
    if (ferr == FS_OK)
        kprintf("Zapisano %s (%u B)\n", fname, r.body_len);
    else
        kprintf("Nie udalo sie zapisac %s (blad %d)\n", fname, ferr);
    http_free(&r);
}

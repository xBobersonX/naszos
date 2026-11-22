/* Komenda "js" w terminalu - JavaScript bez strony WWW (src/js_*.c).
 *   js 2 ** 10              - policz wyrazenie i wypisz wynik
 *   js skrypt.js            - wykonaj plik (console.log pisze w terminalu)
 *   js                      - tryb interaktywny: kazda linia to kawalek
 *                             programu, zmienne zostaja; pusta linia konczy */

#include <stdint.h>
#include "shell_media.h"
#include "shell.h"
#include "js.h"
#include "fs.h"
#include "vga.h"
#include "string.h"
#include "utf8.h"
#include "thread.h"

static void term_console(void *ctx, int level, const char *text)
{
    (void)ctx;
    char buf[1024];
    utf8_to_ascii(text, (unsigned)strlen(text), buf, sizeof(buf));   /* terminal VGA: bez ogonkow */
    if (level == 2)
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
    else if (level == 1)
        vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_write(buf);
    vga_write("\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void term_alert(void *ctx, const char *text)
{
    term_console(ctx, 0, text);
}

static void eval_line(struct js_rt *rt, const char *code)
{
    char out[1024], ascii[1024];
    if (js_eval_text(rt, code, (unsigned)strlen(code), out, sizeof(out)) == 0 && strcmp(out, "undefined") != 0) {
        utf8_to_ascii(out, (unsigned)strlen(out), ascii, sizeof(ascii));
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        vga_write(ascii);
        vga_write("\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

void cmd_js(char *args)
{
    while (*args == ' ')
        args++;
    struct js_host host;
    memset(&host, 0, sizeof(host));
    host.console = term_console;
    host.alert = term_alert;
    host.view_w = 800;
    host.view_h = 600;
    struct js_rt *rt = js_new(0, 0, "", &host);
    if (!rt) {
        vga_write("Za malo pamieci na JavaScript.\n");
        return;
    }
    js_set_cancel(rt, &con_cur->closed);
    unsigned n = (unsigned)strlen(args);
    if (n > 3 && strcmp(args + n - 3, ".js") == 0) {
        struct fs_node *f, *cwd = fs_node_valid(con_cur->cwd) ? con_cur->cwd : fs_root();
        if (fs_lookup(cwd, args, &f) != FS_OK || f->is_dir) {
            kprintf("Nie ma pliku %s\n", args);
        } else {
            js_run(rt, f->data, f->size, args, 0);
            while (js_has_timers(rt) && !con_cur->closed) {  /* setTimeout w skrypcie */
                js_timers(rt);
                cpu_idle();
            }
        }
    } else if (*args) {
        eval_line(rt, args);
    } else {
        vga_write("JavaScript NaszOS. Wpisz kod (np. [1,2,3].map(x => x * 2)), pusta linia - koniec.\n");
        for (;;) {
            char line[128];
            vga_set_color(VGA_YELLOW, VGA_BLACK);
            vga_write("js> ");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            shell_read_line(line);
            if (!line[0] || con_cur->closed)
                break;
            eval_line(rt, line);
            js_timers(rt);
        }
    }
    js_free(rt);
}

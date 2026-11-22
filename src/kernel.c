#include <stdint.h>
#include "kernel.h"
#include "multiboot.h"
#include "vga.h"
#include "gdt.h"
#include "idt.h"
#include "timer.h"
#include "keyboard.h"
#include "pmm.h"
#include "heap.h"
#include "fs.h"
#include "ata.h"
#include "storage.h"
#include "shell.h"
#include "paging.h"
#include "process.h"
#include "speaker.h"
#include "gui.h"
#include "rtc.h"
#include "mouse.h"
#include "thread.h"
#include "net.h"
#include "shell_net.h"
#include "audio.h"
#include "ac97.h"

void panic(const char *msg)
{
    __asm__ volatile ("cli");
    vga_emergency();
    vga_set_color(VGA_WHITE, VGA_RED);
    kprintf("\n\n *** KERNEL PANIC: %s *** \n", msg);
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_write("\nSystem zatrzymany.");
    for (;;)
        __asm__ volatile ("hlt");
}

static void ok(void)
{
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    vga_write("[ OK ] ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void warn(void)
{
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    vga_write("[UWAGA] ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

/* Pasek statusu w wierszu 0: nazwa systemu po lewej, data i godzina
 * na srodku, czas pracy po prawej */
static void draw_status_bar(void)
{
    vga_fill_row(0, VGA_BLACK, VGA_LIGHT_GREY);
    vga_write_at(1, 0, OS_NAME " v" OS_VERSION, VGA_BLUE, VGA_LIGHT_GREY);
}

static void put2(char *p, uint32_t v)
{
    p[0] = (char)('0' + v / 10 % 10);
    p[1] = (char)('0' + v % 10);
}

/* Wolane z przerwania zegara kilka razy na sekunde */
static void draw_clock(uint32_t uptime_secs)
{
    struct rtc_time t;
    rtc_read(&t);
    char now[] = "Pn 01.01.2000 00:00:00";
    now[0] = rtc_day_short[t.weekday][0];
    now[1] = rtc_day_short[t.weekday][1];
    put2(now + 3, t.day);
    put2(now + 6, t.month);
    put2(now + 9, t.year / 100);
    put2(now + 11, t.year % 100);
    put2(now + 14, t.hour);
    put2(now + 17, t.minute);
    put2(now + 20, t.second);
    /* Z przerwania - moze dzialac dowolne zadanie, wiec piszemy jawnie na ekran */
    unsigned cols = vga_screen_width();
    vga_status_write((cols - (sizeof(now) - 1)) / 2, now, VGA_BLACK, VGA_LIGHT_GREY);

    char up[] = "Czas pracy: 00:00:00 ";
    put2(up + 12, uptime_secs / 3600 % 100);
    put2(up + 15, uptime_secs / 60 % 60);
    put2(up + 18, uptime_secs % 60);
    vga_status_write(cols - (sizeof(up) - 1), up, VGA_BLACK, VGA_LIGHT_GREY);

    vga_mouse_refresh();                /* pasek albo inny tekst mogl nadpisac kursor myszy */
}

static unsigned parse_number(const char **s)
{
    unsigned v = 0;
    while (**s >= '0' && **s <= '9' && v < 100000)
        v = v * 10 + (unsigned)(*(*s)++ - '0');
    return v;
}

/* Szuka w linii polecen "res=1920x1200" */
static int parse_resolution(const char *cmdline, unsigned *w, unsigned *h)
{
    for (const char *p = cmdline; *p; p++) {
        if (p[0] == 'r' && p[1] == 'e' && p[2] == 's' && p[3] == '=') {
            p += 4;
            *w = parse_number(&p);
            if (*p++ != 'x')
                return 0;
            *h = parse_number(&p);
            return *w && *h;
        }
    }
    return 0;
}

static volatile int breakpoint_hit;

static void on_breakpoint(struct regs *r)
{
    (void)r;
    breakpoint_hit = 1;
}

void kernel_main(uint32_t magic, struct multiboot_info *mbi)
{
    vga_init();
    draw_status_bar();

    if (magic != MULTIBOOT_BOOTLOADER_MAGIC)
        panic("Niepoprawny magic bootloadera - uruchom przez Multiboot");
    ok();
    vga_write("Uruchomiono przez bootloader Multiboot, procesor w trybie 64-bitowym\n");

    /* Parametr "res=SZERxWYS" z linii polecen kernela - tryb graficzny */
    unsigned res_w, res_h;
    if ((mbi->flags & MULTIBOOT_INFO_CMDLINE) &&
        parse_resolution((const char *)(uintptr_t)mbi->cmdline, &res_w, &res_h)) {
        if (vga_set_graphics(res_w, res_h)) {
            draw_status_bar();
            ok();
            kprintf("Tryb graficzny %ux%u - ekran ma %ux%u znakow\n",
                    res_w, res_h, vga_width(), vga_height());
        } else {
            warn();
            kprintf("Karta nie obsluguje trybu %ux%u - zostaje tryb tekstowy 80x25\n",
                    res_w, res_h);
        }
    }

    thread_init();
    gdt_init();
    ok();
    vga_write("GDT zaladowana (segmenty kernela i programow)\n");

    idt_init();
    process_init();
    ok();
    vga_write("IDT zaladowana, PIC przestawiony na wektory 32-47, int 0x80 dla programow\n");

    /* Test: wywolujemy przerwanie programowo i sprawdzamy, czy wrocilismy */
    interrupt_register(3, on_breakpoint);
    __asm__ volatile ("int $3");
    if (breakpoint_hit) {
        ok();
        vga_write("Test przerwania int3 - obsluzone, system dziala dalej\n");
    }

    pmm_init(mbi);
    ok();
    kprintf("Pamiec fizyczna: %u MiB, wolne %u ramek po 4 KiB\n",
            pmm_total_frames() * (FRAME_SIZE / 1024) / 1024,
            pmm_total_frames() - pmm_used_frames());

    paging_init(pmm_top());
    ok();
    vga_write("Stronicowanie 4-poziomowe wlaczone - programy dostana wlasna, chroniona pamiec\n");

    heap_init();
    struct heap_stats hs;
    heap_get_stats(&hs);
    ok();
    kprintf("Sterta kernela: %u KiB pod adresem 0x%lx\n", hs.total / 1024, pmm_first_free_addr());

    fs_init();
    if (storage_init()) {
        ok();
        kprintf("Dysk ATA: %s, %u KiB\n", ata_model(), ata_sectors() / 2);
        uint32_t entries = 0;
        int err = storage_load(&entries);
        if (err == STORAGE_OK) {
            ok();
            kprintf("Wczytano z dysku %u plikow i katalogow\n", entries);
        } else {
            if (err == STORAGE_EMPTY) {
                ok();
                vga_write("Dysk jest pusty - tworze nowy system plikow\n");
            } else {
                warn();
                kprintf("Nie udalo sie wczytac plikow: %s\n", storage_strerror(err));
            }
            fs_seed();
            storage_save();
        }
    } else {
        warn();
        vga_write("Brak dysku - pliki beda tylko w RAM i znikna po wylaczeniu\n");
        fs_seed();
    }

    int programs = process_import_modules(mbi);
    if (programs > 0) {
        storage_sync_if_dirty();
        ok();
        kprintf("Zainstalowano %u programow w /programy\n", programs);
    }

    gui_settings_load();                /* m.in. czy dzwieki sa wlaczone */

    draw_clock(0);
    timer_on_refresh(draw_clock);
    timer_init();
    keyboard_init();
    int have_mouse = mouse_init();      /* przed sti - odpowiedzi myszy nie moga trafic do przerwan */
    __asm__ volatile ("sti");
    ok();
    vga_write("Przerwania wlaczone\n");
    ok();
    kprintf("Zegar PIT: %u Hz\n", TIMER_HZ);
    int card = audio_init();
    sound_startup();                    /* melodyjka startowa gra w tle */
    ok();
    if (card)
        kprintf("Karta dzwiekowa AC'97: %u Hz stereo, dzwieki %s\n", AC97_RATE,
                sound_enabled() ? "wlaczone" : "wylaczone");
    else
        vga_write(sound_enabled() ? "Glosniczek PC: dzwieki wlaczone\n" : "Glosniczek PC: dzwieki wylaczone\n");

    /* Test: 500 ms snu powinno dac ok. 50 tykniec */
    uint32_t start = timer_ticks();
    timer_sleep(500);
    ok();
    kprintf("Test sleep(500 ms): minelo %u tykniec\n", timer_ticks() - start);
    ok();
    vga_write("Klawiatura PS/2 gotowa\n");
    if (have_mouse) {
        ok();
        vga_write(mouse_has_wheel() ? "Mysz PS/2 gotowa (z kolkiem)\n" : "Mysz PS/2 gotowa\n");
    } else {
        warn();
        vga_write("Mysz nie odpowiada - dziala tylko klawiatura\n");
    }

    if (net_init()) {
        const struct net_info *n = net_info();
        char ip[16];
        net_format_ip(n->ip, ip);
        ok();
        kprintf("Siec: karta RTL8139, adres IP %s (%s)\n", ip, n->dhcp ? "z DHCP" : "ustawiony recznie");
        int64_t diff;
        struct rtc_time t;
        if (net_clock_sync(&diff, &t) == 0) {
            ok();
            kprintf("Zegar zsynchronizowany z internetem: %02u:%02u:%02u", t.hour, t.minute, t.second);
            if (diff)
                kprintf(" (%s o %ld s - poprawiony)", diff > 0 ? "spieszyl sie" : "spoznial sie",
                        diff > 0 ? diff : -diff);
            vga_putc('\n');
        } else {
            warn();
            vga_write("Nie udalo sie pobrac czasu z internetu\n");
        }
    } else {
        warn();
        vga_write("Brak karty sieciowej - bez internetu\n");
    }

    shell_run();
}

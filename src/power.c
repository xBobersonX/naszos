#include "power.h"
#include "io.h"
#include "timer.h"
#include "storage.h"
#include "vga.h"
#include "speaker.h"
#include "string.h"

#define QEMU_EXIT_PORT   0xF4     /* urzadzenie isa-debug-exit (dodaje je build.ps1) */
#define QEMU_EXIT_REBOOT 0x10     /* QEMU konczy sie z kodem (0x10 << 1) | 1 = 33 */

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

/* Czy dzialamy w QEMU? Bit 31 ECX z CPUID(1) = "jestem maszyna wirtualna",
 * a CPUID(0x40000000) podaje nazwe: "TCGTCGTCGTCG" albo "KVMKVMKVM" */
static int running_in_qemu(void)
{
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    if (!(c & (1u << 31)))
        return 0;
    cpuid(0x40000000, &a, &b, &c, &d);
    char vendor[13];
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &c, 4);
    memcpy(vendor + 8, &d, 4);
    vendor[12] = '\0';
    return memcmp(vendor, "TCGTCGTCGTCG", 12) == 0 || memcmp(vendor, "KVMKVMKVM", 9) == 0;
}

void system_reboot(void)
{
    storage_sync_if_dirty();
    sound_shutdown();
    sound_wait();                 /* pozegnanie musi wybrzmiec przed restartem */
    vga_write("Restart...\n");
    timer_sleep(300);
    /* W QEMU: zamiast resetu dajemy znac skryptowi build.ps1, ze to restart - on
     * uruchomi QEMU od nowa i ustawi pelny ekran (sam reset rozciagal obraz).
     * Bez urzadzenia isa-debug-exit ten zapis nic nie robi i dzialamy dalej. */
    if (running_in_qemu())
        outb(QEMU_EXIT_PORT, QEMU_EXIT_REBOOT);
    __asm__ volatile ("cli");
    while (inb(0x64) & 0x02)      /* czekamy, az kontroler klawiatury bedzie gotowy */
        ;
    outb(0x64, 0xFE);             /* impuls na linii RESET procesora */
    for (;;)
        __asm__ volatile ("hlt");
}

void system_shutdown(void)
{
    storage_sync_if_dirty();
    sound_shutdown();
    sound_wait();
    vga_write("Wylaczanie...\n");
    timer_sleep(300);
    outw(0x604, 0x2000);          /* ACPI w QEMU */
    outw(0xB004, 0x2000);         /* starsze QEMU / Bochs */
    vga_write("Tego komputera nie da sie wylaczyc programowo - mozna go juz wylaczyc.\n");
    __asm__ volatile ("cli");
    for (;;)
        __asm__ volatile ("hlt");
}

/* Menedzer pamieci fizycznej. Pamiec dzielimy na ramki po 4 KiB, a stan
 * kazdej ramki (1 = zajeta, 0 = wolna) trzymamy w mapie bitowej, ktora
 * lezy tuz za kernelem. Wolne obszary bierzemy z mapy pamieci BIOS-u. */

#include "pmm.h"
#include "string.h"
#include "vga.h"
#include "kernel.h"

extern char _kernel_start[], _kernel_end[];

#define MAX_RAM (64ull << 30)   /* tyle pamieci obslugujemy (64 GiB) */

static uint32_t *bitmap;
static uint32_t total_frames, used_frames;
static uint32_t hole_frames;       /* dziury w mapie (np. 3-4 GiB: urzadzenia) - to nie RAM */
static uintptr_t first_free_addr;
static uint64_t ram_top;
static struct multiboot_info *boot_info;

#define ALIGN_UP(x)   (((x) + FRAME_SIZE - 1) & ~(uint64_t)(FRAME_SIZE - 1))
#define ALIGN_DOWN(x) ((x) & ~(uint64_t)(FRAME_SIZE - 1))

static inline int  frame_used(uint32_t f) { return bitmap[f / 32] >> (f % 32) & 1; }

static void mark_used(uint32_t f)
{
    if (!frame_used(f)) {
        bitmap[f / 32] |= 1u << (f % 32);
        used_frames++;
    }
}

static void mark_free(uint32_t f)
{
    if (frame_used(f)) {
        bitmap[f / 32] &= ~(1u << (f % 32));
        used_frames--;
    }
}

static void reserve_range(uint64_t start, uint64_t end)
{
    for (uint64_t f = ALIGN_DOWN(start) / FRAME_SIZE;
         f < ALIGN_UP(end) / FRAME_SIZE && f < total_frames; f++)
        mark_used(f);
}

/* W trybie 64-bitowym widzimy cala pamiec - takze powyzej 4 GiB. Obcinamy
 * tylko do MAX_RAM (tyle mapuje paging.c). 0 = region pusty. */
static int clip_region(const struct multiboot_mmap_entry *e, uint64_t *start, uint64_t *end)
{
    if (e->addr >= MAX_RAM)
        return 0;
    uint64_t e_end = e->addr + e->len;
    *start = e->addr;
    *end = e_end > MAX_RAM ? MAX_RAM : e_end;
    return *end > *start;
}

#define FOR_EACH_MMAP(e, mbi)                                                   \
    for (struct multiboot_mmap_entry *e = (void *)(uintptr_t)(mbi)->mmap_addr;  \
         (uintptr_t)e < (uintptr_t)(mbi)->mmap_addr + (mbi)->mmap_length;       \
         e = (void *)((uintptr_t)e + e->size + sizeof(e->size)))

void pmm_init(struct multiboot_info *mbi)
{
    boot_info = mbi;
    int have_mmap = mbi->flags & MULTIBOOT_INFO_MMAP;
    uint64_t start, end, top = 0;

    /* 1. Najwyzszy adres dostepnej pamieci wyznacza rozmiar mapy bitowej */
    if (have_mmap) {
        FOR_EACH_MMAP(e, mbi)
            if (e->type == MULTIBOOT_MEMORY_AVAILABLE && clip_region(e, &start, &end) && end > top)
                top = end;
    } else if (mbi->flags & MULTIBOOT_INFO_MEMORY) {
        top = 0x100000 + (uint64_t)mbi->mem_upper * 1024;
    }
    if (top == 0)
        panic("Bootloader nie przekazal informacji o pamieci");

    ram_top = top;
    total_frames = (uint32_t)(ALIGN_DOWN(top) / FRAME_SIZE);
    uint32_t bitmap_bytes = (total_frames + 31) / 32 * 4;

    /* Bootloader wgrywa moduly (programy .exe) zaraz za kernelem - mapa bitowa
     * i sterta musza zaczynac sie za nimi, inaczej bysmy je nadpisali */
    uintptr_t after_kernel = (uintptr_t)_kernel_end;
    const struct multiboot_module *mods = (const void *)(uintptr_t)mbi->mods_addr;
    uint32_t mods_count = (mbi->flags & MULTIBOOT_INFO_MODS) ? mbi->mods_count : 0;
    for (uint32_t i = 0; i < mods_count; i++)
        if (mods[i].mod_end > after_kernel)
            after_kernel = mods[i].mod_end;
    bitmap = (uint32_t *)ALIGN_UP(after_kernel);
    first_free_addr = ALIGN_UP((uintptr_t)bitmap + bitmap_bytes);

    /* 2. Na poczatku wszystko zajete, potem zwalniamy to, co BIOS zglasza jako dostepne */
    memset(bitmap, 0xFF, bitmap_bytes);
    used_frames = total_frames;

    if (have_mmap) {
        FOR_EACH_MMAP(e, mbi) {
            if (e->type != MULTIBOOT_MEMORY_AVAILABLE || !clip_region(e, &start, &end))
                continue;
            for (uint64_t f = ALIGN_UP(start) / FRAME_SIZE;
                 f < ALIGN_DOWN(end) / FRAME_SIZE && f < total_frames; f++)
                mark_free(f);
        }
    } else {
        for (uint32_t f = 0x100000 / FRAME_SIZE; f < total_frames; f++)
            mark_free(f);
    }

    /* Wszystko, co zostalo zajete po kroku 2, to nie jest RAM */
    hole_frames = used_frames;

    /* 3. Obszary, ktorych nie wolno rozdawac */
    reserve_range(0, 0x100000);                              /* BIOS, VGA, dane bootloadera */
    reserve_range((uintptr_t)_kernel_start, first_free_addr); /* kernel, moduly, mapa bitowa */
    for (uint32_t i = 0; i < mods_count; i++)
        reserve_range(mods[i].mod_start, mods[i].mod_end);
}

/* Szukamy od konca pamieci: sterta kernela rosnie w gore, zajmujac ramki
 * zaraz za soba (pmm_claim) - gdybysmy rozdawali najnizsze wolne ramki,
 * trafilibysmy dokladnie w miejsce, w ktore sterta chce urosnac */
uintptr_t pmm_alloc(void)
{
    for (uint32_t i = (total_frames + 31) / 32; i-- > 0;) {
        if (bitmap[i] == 0xFFFFFFFF)          /* 32 zajete ramki - pomijamy od razu */
            continue;
        for (uint32_t b = 32; b-- > 0;) {
            uint32_t f = i * 32 + b;
            if (f < total_frames && !frame_used(f)) {
                mark_used(f);
                return (uintptr_t)f * FRAME_SIZE;
            }
        }
    }
    return 0;
}

void pmm_free(uintptr_t addr)
{
    uint64_t f = addr / FRAME_SIZE;
    if (addr % FRAME_SIZE || f >= total_frames || !frame_used(f))
        panic("pmm_free: niepoprawny adres ramki");
    mark_free(f);
}

int pmm_claim(uintptr_t addr)
{
    uint64_t f = addr / FRAME_SIZE;
    if (f >= total_frames || frame_used(f))
        return 0;
    mark_used(f);
    return 1;
}

/* Statystyki licza tylko prawdziwy RAM, bez dziur w przestrzeni adresowej */
uint32_t pmm_total_frames(void)    { return total_frames - hole_frames; }
uint32_t pmm_used_frames(void)     { return used_frames - hole_frames; }
uintptr_t pmm_first_free_addr(void) { return first_free_addr; }
uint64_t pmm_top(void)             { return ram_top; }

void pmm_print_map(void)
{
    if (!(boot_info->flags & MULTIBOOT_INFO_MMAP)) {
        vga_write("Bootloader nie przekazal mapy pamieci.\n");
        return;
    }
    vga_write("Poczatek     Koniec            Rozmiar  Typ\n");
    FOR_EACH_MMAP(e, boot_info) {
        uint64_t last = e->addr + e->len - 1;
        uint64_t kib = e->len >> 10;
        kprintf("0x%09lx  0x%09lx  %9lu KiB  ", e->addr, last, kib);
        if (e->type == MULTIBOOT_MEMORY_AVAILABLE) {
            vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
            vga_write("dostepna\n");
        } else {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            kprintf("zarezerwowana (%u)\n", e->type);
        }
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

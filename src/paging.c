/* Stronicowanie - patrz paging.h. Kazda tablica ma 512 wpisow po 8 bajtow.
 * Adres wirtualny dzieli sie na 4 indeksy po 9 bitow (PML4, PDPT, PD, PT)
 * i 12 bitow przesuniecia w stronie 4 KiB:
 *   bity 39-47: PML4 (po 512 GiB), 30-38: PDPT (po 1 GiB),
 *   bity 21-29: PD (po 2 MiB),     12-20: PT (po 4 KiB).
 * Czesc systemowa konczy sie na PD - wpisy PD z bitem PS to strony 2 MiB. */

#include "paging.h"
#include "pmm.h"
#include "string.h"

#define PTE_PRESENT 0x001ull
#define PTE_WRITE   0x002ull
#define PTE_USER    0x004ull
#define PTE_LARGE   0x080ull                 /* strona 2 MiB (we wpisie PD) */
#define PTE_ADDR    0x000FFFFFFFFFF000ull    /* bity adresu w kazdym wpisie */

#define MAX_GIB     64                       /* tyle pamieci mapujemy najwyzej */

static uint64_t kernel_pml4[512] __attribute__((aligned(4096)));
static uint64_t kernel_pdpt[512] __attribute__((aligned(4096)));
static uint64_t kernel_pd[MAX_GIB][512] __attribute__((aligned(4096)));
static uint64_t current_dir;

static inline unsigned pml4_index(uint64_t a) { return (a >> 39) & 511; }
static inline unsigned pdpt_index(uint64_t a) { return (a >> 30) & 511; }
static inline unsigned pd_index(uint64_t a)   { return (a >> 21) & 511; }
static inline unsigned pt_index(uint64_t a)   { return (a >> 12) & 511; }

static inline uint64_t *table_of(uint64_t entry)
{
    return (uint64_t *)(uintptr_t)(entry & PTE_ADDR);   /* ramki sa zmapowane 1:1 */
}

void paging_init(uint64_t ram_top)
{
    /* Mapujemy co najmniej 4 GiB - pod koniec tego obszaru leza urzadzenia
     * (m.in. bufor ramki karty graficznej) - i cala pamiec RAM powyzej */
    unsigned gib = (unsigned)((ram_top + (1ull << 30) - 1) >> 30);
    if (gib < 4)
        gib = 4;
    if (gib > MAX_GIB)
        gib = MAX_GIB;

    memset(kernel_pml4, 0, sizeof(kernel_pml4));
    memset(kernel_pdpt, 0, sizeof(kernel_pdpt));
    for (unsigned g = 0; g < gib; g++) {
        for (unsigned i = 0; i < 512; i++) {
            uint64_t addr = ((uint64_t)g << 30) + ((uint64_t)i << 21);
            kernel_pd[g][i] = addr | PTE_PRESENT | PTE_WRITE | PTE_LARGE;
        }
        kernel_pdpt[g] = (uintptr_t)kernel_pd[g] | PTE_PRESENT | PTE_WRITE;
    }
    /* Bez PTE_USER: program w ring 3 nie siegnie do pamieci systemu */
    kernel_pml4[0] = (uintptr_t)kernel_pdpt | PTE_PRESENT | PTE_WRITE;

    /* Stronicowanie juz dziala (wlaczyl je boot.S) - podmieniamy tylko tablice */
    current_dir = (uintptr_t)kernel_pml4;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(current_dir) : "memory");
}

uint64_t paging_kernel_dir(void)
{
    return (uintptr_t)kernel_pml4;
}

void paging_switch(uint64_t dir)
{
    if (!dir)
        dir = (uintptr_t)kernel_pml4;
    if (dir == current_dir)
        return;
    current_dir = dir;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(dir) : "memory");
}

static uint64_t zeroed_frame(void)
{
    uint64_t frame = pmm_alloc();
    if (frame)
        memset((void *)(uintptr_t)frame, 0, PAGE_SIZE);
    return frame;
}

uint64_t paging_new_dir(void)
{
    uint64_t frame = zeroed_frame();
    if (!frame)
        return 0;
    uint64_t *pml4 = (uint64_t *)(uintptr_t)frame;
    pml4[0] = kernel_pml4[0];                   /* wspolna czesc systemowa */
    return frame;
}

/* Zwalnia tablice poziomu "level" (3 = PDPT, 2 = PD, 1 = PT) razem ze
 * wszystkim, na co wskazuje, lacznie ze stronami programu */
static void free_table(uint64_t *table, int level)
{
    for (int i = 0; i < 512; i++) {
        if (!(table[i] & PTE_PRESENT))
            continue;
        if (level > 1)
            free_table(table_of(table[i]), level - 1);
        else
            pmm_free(table[i] & PTE_ADDR);
    }
    pmm_free((uintptr_t)table);
}

void paging_free_dir(uint64_t dir)
{
    uint64_t *pml4 = (uint64_t *)(uintptr_t)dir;
    uint64_t e = pml4[pml4_index(USER_BASE)];
    if (e & PTE_PRESENT)
        free_table(table_of(e), 3);
    pmm_free(dir);
}

/* Zwraca tablice nastepnego poziomu dla wpisu table[idx], w razie potrzeby
 * tworzac ja (create = 1). 0 = brak / brak pamieci. */
static uint64_t *next_table(uint64_t *table, unsigned idx, int create)
{
    if (!(table[idx] & PTE_PRESENT)) {
        if (!create)
            return 0;
        uint64_t frame = zeroed_frame();
        if (!frame)
            return 0;
        table[idx] = frame | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    return table_of(table[idx]);
}

/* Wpis PT dla adresu vaddr w przestrzeni programu (0 = brak) */
static uint64_t *user_pte(uint64_t dir, uint64_t vaddr, int create)
{
    uint64_t *t = (uint64_t *)(uintptr_t)dir;
    if (!(t = next_table(t, pml4_index(vaddr), create)))
        return 0;
    if (!(t = next_table(t, pdpt_index(vaddr), create)))
        return 0;
    if (!(t = next_table(t, pd_index(vaddr), create)))
        return 0;
    return &t[pt_index(vaddr)];
}

int paging_map_user(uint64_t dir, uint64_t vaddr)
{
    if (vaddr < USER_BASE || vaddr >= USER_END)
        return 0;
    uint64_t *pte = user_pte(dir, vaddr, 1);
    if (!pte)
        return 0;
    if (*pte & PTE_PRESENT)
        return 1;                               /* juz zmapowana */
    uint64_t frame = zeroed_frame();
    if (!frame)
        return 0;
    *pte = frame | PTE_PRESENT | PTE_WRITE | PTE_USER;
    return 1;
}

int paging_user_range_ok(uint64_t dir, uint64_t ptr, uint64_t len)
{
    if (ptr < USER_BASE || ptr >= USER_END || len > USER_END - ptr)
        return 0;
    if (len == 0)
        len = 1;
    uint64_t first = ptr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t last = (ptr + len - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    for (uint64_t page = first;; page += PAGE_SIZE) {
        uint64_t *pte = user_pte(dir, page, 0);
        if (!pte || !(*pte & PTE_PRESENT))
            return 0;
        if (page == last)
            return 1;
    }
}

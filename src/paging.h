#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

/* Pamiec wirtualna (x86-64, 4 poziomy tablic: PML4 -> PDPT -> PD -> PT).
 * Kernel widzi cala pamiec fizyczna "jeden do jednego" (adres wirtualny =
 * fizyczny), w duzych stronach 2 MiB dostepnych tylko dla systemu - wpis 0
 * tablicy PML4 (pierwsze 512 GiB adresow). Program uzytkownika dostaje
 * wlasna tablice PML4: czesc systemowa jest wspolna (ale niedostepna
 * z ring 3), a jego pamiec lezy we wpisie 1 (od 512 GiB), w obszarze
 * USER_BASE..USER_END, niewidocznym dla innych programow. */

#define USER_BASE  0x8000000000ull               /* 512 GiB */
#define USER_END   (USER_BASE + 0x40000000ull)   /* +1 GiB dla programu */
#define PAGE_SIZE  4096u

/* ram_top = koniec najwyzszego obszaru RAM (moze byc powyzej 4 GiB) */
void paging_init(uint64_t ram_top);
uint64_t paging_kernel_dir(void);
void paging_switch(uint64_t dir);          /* ladowanie CR3, gdy sie zmienia */

uint64_t paging_new_dir(void);             /* 0 = brak pamieci */
void paging_free_dir(uint64_t dir);        /* razem ze stronami programu */
/* Mapuje wyzerowana strone pod adresem vaddr (w obszarze uzytkownika). 1 = OK */
int paging_map_user(uint64_t dir, uint64_t vaddr);

/* Czy [ptr, ptr+len) lezy w pamieci biezacego programu (zmapowanej)? */
int paging_user_range_ok(uint64_t dir, uint64_t ptr, uint64_t len);

#endif

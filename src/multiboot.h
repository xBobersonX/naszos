#ifndef MULTIBOOT_H
#define MULTIBOOT_H

#include <stdint.h>

#define MULTIBOOT_BOOTLOADER_MAGIC 0x2BADB002

#define MULTIBOOT_INFO_MEMORY  (1 << 0)
#define MULTIBOOT_INFO_CMDLINE (1 << 2)
#define MULTIBOOT_INFO_MODS    (1 << 3)
#define MULTIBOOT_INFO_MMAP    (1 << 6)

/* Modul = dodatkowy plik wczytany przez bootloader (u nas: programy .exe) */
struct multiboot_module {
    uint32_t mod_start;       /* adres fizyczny poczatku */
    uint32_t mod_end;         /* adres tuz za koncem */
    uint32_t string;          /* nazwa (sciezka podana bootloaderowi) */
    uint32_t reserved;
} __attribute__((packed));

#define MULTIBOOT_MEMORY_AVAILABLE 1

/* Struktura przekazywana przez bootloader (specyfikacja Multiboot 0.6.96) */
struct multiboot_info {
    uint32_t flags;
    uint32_t mem_lower;       /* KiB ponizej 1 MiB */
    uint32_t mem_upper;       /* KiB powyzej 1 MiB */
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count;
    uint32_t mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length;     /* mapa pamieci od BIOS-u */
    uint32_t mmap_addr;
} __attribute__((packed));

struct multiboot_mmap_entry {
    uint32_t size;            /* rozmiar wpisu BEZ tego pola */
    uint64_t addr;
    uint64_t len;
    uint32_t type;            /* 1 = dostepna, reszta zarezerwowana */
} __attribute__((packed));

#endif

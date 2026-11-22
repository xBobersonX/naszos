#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>

struct regs;

/* Programy uzytkownika (pliki .exe w formacie ELF, 64 bity). Program dziala
 * w ring 3, we wlasnej przestrzeni adresowej, i rozmawia z systemem przez
 * przerwanie int 0x80 (numer uslugi w RAX, argumenty w RDI, RSI, RDX, R10,
 * R8 - prawie jak zwykle wywolanie funkcji w x86-64, wynik w RAX). */

void process_init(void);

/* Uruchamia program w biezacym zadaniu i czeka na jego koniec. Zwraca kod
 * wyjscia programu albo PROCESS_E* przy bledzie. Opis bledu do *error. */
#define PROCESS_ENOFILE  -1000
#define PROCESS_EFORMAT  -1001
#define PROCESS_ENOMEM   -1002
#define PROCESS_CRASHED  -1003
#define PROCESS_KILLED   -1004
int process_run(const char *path, const char **error);

/* Uruchamia program w nowym zadaniu z ta sama konsola i czeka (powloka) */
int process_run_and_wait(const char *path);

/* Z obslugi przerwan: wyjatek w programie - zamyka go (nie wraca) */
void process_fault(struct regs *r, const char *what);
/* Z przerwania zegara, gdy przerwany zostal program: zamkniete okno konczy
 * program, a po kilku tyknieciach procesor dostaja inne zadania */
void process_tick(void);

/* Programy dostarczone przez bootloader (moduly) -> /programy */
struct multiboot_info;
int process_import_modules(struct multiboot_info *mbi);

#endif

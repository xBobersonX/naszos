#ifndef THREAD_H
#define THREAD_H

#include <stdint.h>

struct console;

/* Wielozadaniowosc "wspolpracujaca": zadania (watki) dzialaja na zmiane,
 * a przelaczenie nastepuje, gdy zadanie samo odda procesor - thread_yield()
 * albo cpu_idle() (np. gdy czeka na klawisz). Zadanie 0 to glowne zadanie
 * kernela (powloka na ekranie / pulpit). Kazde zadanie ma wlasna konsole -
 * przelaczenie zadania przelacza tez con_cur. */

void thread_init(void);
int  thread_create(void (*fn)(void *), void *arg, struct console *con);   /* id albo -1 */
void thread_yield(void);
void thread_exit(void) __attribute__((noreturn));
int  thread_alive(int id);
int  thread_is_main(void);

/* Z przerwania zegara, gdy przerwany zostal program uzytkownika: po kilku
 * tyknieciach oddaje procesor innym zadaniom (wywlaszczanie). */
void thread_preempt(void);

/* Program uzytkownika w biezacym zadaniu: jego katalog stron (0 = kernela) */
void thread_set_address_space(uint64_t dir, void *process);
void *thread_process(void);
/* Stos kernela, na ktory procesor przechodzi przy przerwaniu z programu.
 * Musi lezec PONIZEJ ramek funkcji, ktore uruchomily program - inaczej
 * kazde wywolanie systemowe by je nadpisalo. */
void thread_set_user_kernel_stack(uint64_t rsp0);

/* Czekanie "na cokolwiek": oddaje procesor innym zadaniom, a zadanie
 * glowne dodatkowo usypia procesor do nastepnego przerwania. */
void cpu_idle(void);

#endif

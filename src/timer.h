#ifndef TIMER_H
#define TIMER_H

#include <stdint.h>

#define TIMER_HZ 100

void timer_init(void);
uint32_t timer_ticks(void);        /* tykniecia od startu */
uint32_t timer_uptime_ms(void);
void timer_sleep(uint32_t ms);

#define TIMER_REFRESH_HZ 5

/* Funkcja wolana z przerwania zegara TIMER_REFRESH_HZ razy na sekunde (do
 * odswiezania paska statusu). Musi byc krotka - dziala z wylaczonymi przerwaniami. */
void timer_on_refresh(void (*callback)(uint32_t uptime_secs));

#endif

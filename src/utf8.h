#ifndef UTF8_H
#define UTF8_H

#include <stdint.h>

/* UTF-8: jak zapisac dowolny znak (np. polskie "a" z ogonkiem) bajtami.
 * Znaki ASCII to 1 bajt, polskie litery - 2 bajty, inne nawet 3-4. */

/* Dekoduje znak spod *s (nie dalej niz end) i przesuwa *s za niego */
uint32_t utf8_next(const char **s, const char *end);
/* Zapisuje znak cp do out (najwyzej 4 bajty). Zwraca liczbe bajtow */
unsigned utf8_put(uint32_t cp, char *out);
/* Tekst dla czcionki VGA (terminal, paski okien): polskie litery bez
 * ogonkow, inne nieznane znaki jako '?'. Zwraca dlugosc wyniku */
unsigned utf8_to_ascii(const char *in, unsigned len, char *out, unsigned size);
/* Znak Unicode -> najblizszy znak ASCII (0 = pominac) */
char unicode_to_ascii(uint32_t cp);

#endif

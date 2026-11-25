#ifndef CLIPBOARD_H
#define CLIPBOARD_H

/* Schowek systemowy (src/clipboard.c): jeden tekst UTF-8 wspolny dla
 * wszystkich programow pulpitu (Ctrl+C / Ctrl+V). */

void clipboard_set(const char *text, unsigned len);
/* Tresc schowka (zakonczona zerem) albo "" gdy pusty; *len = dlugosc */
const char *clipboard_get(unsigned *len);

#endif

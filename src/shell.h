#ifndef SHELL_H
#define SHELL_H

/* Uruchamia powloke na ekranie; nigdy nie wraca. */
void shell_run(void) __attribute__((noreturn));

/* Powloka w oknie terminala (zadanie z wlasna konsola) - wraca po zamknieciu okna */
void shell_terminal(void *arg);

/* Czytanie linii z edycja (strzalki, Backspace). buf: co najmniej 80 bajtow */
void shell_read_line(char *buf);

#endif

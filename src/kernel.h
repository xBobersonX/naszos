#ifndef KERNEL_H
#define KERNEL_H

#define OS_NAME    "NaszOS"
#define OS_VERSION "3.0"

/* Blad krytyczny wykryty przez kernel - wypisuje komunikat i zatrzymuje system */
void panic(const char *msg) __attribute__((noreturn));

#endif

/* Test na Windowsie: porty wejscia/wyjscia nie istnieja - atrapy dla rtc.c */
#ifndef IO_H
#define IO_H
#include <stdint.h>
static inline void outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
static inline uint8_t inb(uint16_t port) { (void)port; return 0; }
static inline unsigned long long irq_save(void) { return 0; }
static inline void irq_restore(unsigned long long flags) { (void)flags; }
#endif

#ifndef RTL8139_H
#define RTL8139_H

#include <stdint.h>

/* Sterownik karty sieciowej Realtek RTL8139 (QEMU: -device rtl8139).
 * Karta wysyla i odbiera "ramki Ethernet" - surowe paczki bajtow do 1514 B.
 * Co jest w srodku (IP, TCP...), to juz sprawa net.c. */

int rtl8139_init(uint8_t mac[6]);                 /* 1 = karta znaleziona i gotowa */
int rtl8139_send(const void *frame, unsigned len); /* 1 = wyslano */
/* Kolejna odebrana ramka do buf (najwyzej size bajtow). Zwraca dlugosc albo 0. */
unsigned rtl8139_receive(void *buf, unsigned size);

#endif

#ifndef NET_H
#define NET_H

#include <stdint.h>

/* Siec: stos protokolow NaszOS (src/net.c). Warstwy od dolu:
 *   Ethernet - ramki miedzy kartami w jednej sieci lokalnej (adresy MAC)
 *   ARP      - "kto ma adres IP 10.0.2.2? podaj swoj MAC"
 *   IPv4     - paczki (pakiety) do dowolnego komputera w internecie
 *   ICMP     - ping; UDP - krotkie wiadomosci (DHCP, DNS)
 *   TCP      - niezawodne polaczenie (strony WWW)
 * Adresy IP trzymamy jako liczby: 10.0.2.15 = 0x0A00020F. */

struct net_info {
    int card;                    /* znaleziono karte sieciowa */
    int up;                      /* karta dziala i mamy adres IP */
    int dhcp;                    /* adres dostalismy z DHCP (a nie ustawiony recznie) */
    uint8_t mac[6];
    uint32_t ip, mask, gateway, dns;
    uint32_t rx_frames, tx_frames;
};

/* Szuka karty sieciowej i pobiera adres przez DHCP. 1 = siec dziala */
int net_init(void);
const struct net_info *net_info(void);

/* Tylko jedno zadanie naraz korzysta z sieci (np. dwa terminale) */
void net_lock(void);
void net_unlock(void);

void net_format_ip(uint32_t ip, char buf[16]);
int  net_parse_ip(const char *s, uint32_t *ip);   /* "1.2.3.4" -> 1 = OK */

/* Wyniki: 0 = OK, ujemne = blad (NET_E*) */
#define NET_EDOWN     -1     /* brak sieci */
#define NET_ETIMEOUT  -2     /* brak odpowiedzi */
#define NET_ENOTFOUND -3     /* nie ma takiej nazwy (DNS) */
#define NET_ERESET    -4     /* serwer zerwal polaczenie */
#define NET_EABORT    -5     /* okno zamkniete */
const char *net_strerror(int err);

/* Ping: wysyla "echo" i czeka na odpowiedz. rtt = czas w mikrosekundach, ttl z odpowiedzi */
int net_ping(uint32_t ip, uint16_t seq, uint32_t timeout_ms, uint32_t *rtt_us, unsigned *ttl);

/* DNS: nazwa (np. "google.com") albo adres "1.2.3.4" -> IP */
int net_resolve(const char *name, uint32_t *ip);

/* NTP: aktualny czas z serwera (np. "pool.ntp.org") w sekundach od 1970 (UTC) */
int net_ntp(const char *server, int64_t *unix_secs);

/* TCP - jedno polaczenie naraz */
int  tcp_connect(uint32_t ip, uint16_t port);
int  tcp_send(const void *data, unsigned len);
/* Odbiera do size bajtow. >0 = ile, 0 = serwer zakonczyl polaczenie, <0 = blad */
int  tcp_recv(void *buf, unsigned size, uint32_t timeout_ms);
void tcp_close(void);

#endif

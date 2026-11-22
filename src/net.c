/* Stos sieciowy NaszOS - patrz net.h.
 *
 * Budowa ramki wychodzacej (bajty od poczatku):
 *   0..13  Ethernet (MAC odbiorcy, MAC nadawcy, typ)
 *   14..33 IPv4     (m.in. adres nadawcy i odbiorcy, protokol)
 *   34..   ICMP / UDP / TCP i dane
 * Kazda warstwa wpisuje swoj naglowek w to samo miejsce w buforze.
 *
 * Odbior: net_poll() zabiera ramki od sterownika karty i rozdziela je do
 * obslugi ARP/IP -> ICMP/UDP/TCP. Wolamy go w petlach czekania (wait_for),
 * czyli wtedy, gdy ktos akurat na cos z sieci czeka.
 *
 * Wszystkie liczby w naglowkach sieciowych sa "big endian" (najstarszy bajt
 * pierwszy), a procesor x86 trzyma je odwrotnie - dlatego get16/put16. */

#include "net.h"
#include "rtl8139.h"
#include "timer.h"
#include "thread.h"
#include "vga.h"
#include "string.h"

#define ETH_HDR     14
#define IP_HDR      20
#define IP_DATA     (ETH_HDR + IP_HDR)     /* tu zaczyna sie ICMP/UDP/TCP */
#define FRAME_MAX   1514
#define IP_MAX_DATA (1500 - IP_HDR)

#define ETH_ARP     0x0806
#define ETH_IP      0x0800
#define PROTO_ICMP  1
#define PROTO_TCP   6
#define PROTO_UDP   17

static struct net_info info;
static const uint8_t broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static int in_poll;                  /* obslugujemy odebrana ramke (net_poll) */
static volatile int busy;            /* ktores zadanie korzysta z sieci */
static uint16_t ip_id;
static uint64_t tsc_per_ms;          /* do dokladnego mierzenia czasu pingu */

/* ---------- narzedzia ---------- */

static inline uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* Suma kontrolna internetu: suma 16-bitowych slow "z przeniesieniem w kolko",
 * zanegowana. Odbiorca liczy to samo i sprawdza, czy nic sie nie zepsulo. */
static uint32_t sum_words(const uint8_t *p, unsigned len, uint32_t sum)
{
    for (unsigned i = 0; i + 1 < len; i += 2)
        sum += get16(p + i);
    if (len & 1)
        sum += (uint32_t)p[len - 1] << 8;
    return sum;
}

static uint16_t fold(uint32_t sum)
{
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

void net_format_ip(uint32_t ip, char buf[16])
{
    ksnprintf(buf, 16, "%u.%u.%u.%u", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255);
}

int net_parse_ip(const char *s, uint32_t *ip)
{
    uint32_t result = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9')
            return 0;
        unsigned v = 0;
        for (int digits = 0; *s >= '0' && *s <= '9'; s++) {
            v = v * 10 + (unsigned)(*s - '0');
            if (++digits > 3 || v > 255)
                return 0;
        }
        result = result << 8 | v;
        if (part < 3 && *s++ != '.')
            return 0;
    }
    if (*s)
        return 0;
    *ip = result;
    return 1;
}

const char *net_strerror(int err)
{
    switch (err) {
    case NET_EDOWN:     return "brak polaczenia z siecia";
    case NET_ETIMEOUT:  return "brak odpowiedzi (przekroczony czas)";
    case NET_ENOTFOUND: return "nie ma takiej nazwy";
    case NET_ERESET:    return "serwer odrzucil polaczenie";
    case NET_EABORT:    return "przerwano";
    default:            return "nieznany blad";
    }
}

const struct net_info *net_info(void) { return &info; }

void net_lock(void)
{
    while (busy)
        cpu_idle();
    busy = 1;
}

void net_unlock(void) { busy = 0; }

static void net_poll(void);

/* Czeka (odbierajac ramki), az cond(arg) bedzie prawdziwe albo minie ms.
 * 1 = doczekalismy sie. Zamkniecie okna terminala przerywa czekanie. */
static int wait_for(int (*cond)(void *), void *arg, uint32_t ms)
{
    uint32_t start = timer_uptime_ms();
    for (;;) {
        net_poll();
        if (cond(arg))
            return 1;
        if (con_cur->closed || timer_uptime_ms() - start >= ms)
            return 0;
        cpu_idle();
    }
}

/* ---------- Ethernet i ARP ---------- */

#define ARP_CACHE 16
static struct {
    uint32_t ip;
    uint8_t mac[6];
} arp_cache[ARP_CACHE];
static unsigned arp_next;

static int arp_lookup(uint32_t ip, uint8_t mac[6])
{
    for (int i = 0; i < ARP_CACHE; i++)
        if (arp_cache[i].ip == ip && ip) {
            memcpy(mac, arp_cache[i].mac, 6);
            return 1;
        }
    return 0;
}

static void arp_learn(uint32_t ip, const uint8_t mac[6])
{
    if (!ip)
        return;
    for (int i = 0; i < ARP_CACHE; i++)
        if (arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, mac, 6);
            return;
        }
    arp_cache[arp_next].ip = ip;
    memcpy(arp_cache[arp_next].mac, mac, 6);
    arp_next = (arp_next + 1) % ARP_CACHE;
}

static void eth_send(uint8_t *frame, const uint8_t dst[6], uint16_t type, unsigned len)
{
    memcpy(frame, dst, 6);
    memcpy(frame + 6, info.mac, 6);
    put16(frame + 12, type);
    if (rtl8139_send(frame, ETH_HDR + len))
        info.tx_frames++;
}

static void arp_send(uint16_t op, const uint8_t dst_mac[6], uint32_t dst_ip)
{
    uint8_t frame[ETH_HDR + 28];
    uint8_t *a = frame + ETH_HDR;
    put16(a, 1);                     /* sprzet: Ethernet */
    put16(a + 2, ETH_IP);            /* protokol: IPv4 */
    a[4] = 6;
    a[5] = 4;
    put16(a + 6, op);                /* 1 = pytanie, 2 = odpowiedz */
    memcpy(a + 8, info.mac, 6);
    put32(a + 14, info.ip);
    memcpy(a + 18, op == 1 ? (const uint8_t *)"\0\0\0\0\0\0" : dst_mac, 6);
    put32(a + 24, dst_ip);
    eth_send(frame, dst_mac, ETH_ARP, 28);
}

static void arp_input(const uint8_t *a, unsigned len)
{
    if (len < 28 || get16(a) != 1 || get16(a + 2) != ETH_IP || a[4] != 6 || a[5] != 4)
        return;
    uint32_t sender = get32(a + 14), target = get32(a + 24);
    arp_learn(sender, a + 8);
    if (get16(a + 6) == 1 && info.ip && target == info.ip)
        arp_send(2, a + 8, sender);  /* ktos pyta o nas - odpowiadamy */
}

struct arp_wait { uint32_t ip; uint8_t *mac; };
static int arp_known(void *arg)
{
    struct arp_wait *w = arg;
    return arp_lookup(w->ip, w->mac);
}

/* MAC dla adresu IP: z pamieci podrecznej albo pytamy siec ("kto ma ...?") */
static int arp_resolve(uint32_t ip, uint8_t mac[6])
{
    if (arp_lookup(ip, mac))
        return 1;
    if (in_poll) {                   /* w trakcie obslugi ramki nie czekamy */
        arp_send(1, broadcast_mac, ip);
        return 0;
    }
    struct arp_wait w = { ip, mac };
    for (int attempt = 0; attempt < 3; attempt++) {
        arp_send(1, broadcast_mac, ip);
        if (wait_for(arp_known, &w, 500))
            return 1;
    }
    return 0;
}

/* ---------- IPv4 ---------- */

/* Wysyla pakiet IP; dane (len bajtow) juz leza w frame od IP_DATA */
static int ip_send(uint8_t *frame, uint32_t dst, uint8_t proto, unsigned len)
{
    uint8_t mac[6];
    if (dst == 0xFFFFFFFFu) {
        memcpy(mac, broadcast_mac, 6);
    } else {
        /* W naszej sieci lokalnej - wprost; dalej - przez router (brame) */
        uint32_t hop = (info.mask && ((dst ^ info.ip) & info.mask) == 0) ? dst : info.gateway;
        if (!arp_resolve(hop, mac))
            return 0;
    }
    uint8_t *h = frame + ETH_HDR;
    h[0] = 0x45;                     /* wersja 4, naglowek 5 x 4 bajty */
    h[1] = 0;
    put16(h + 2, (uint16_t)(IP_HDR + len));
    put16(h + 4, ip_id++);
    put16(h + 6, 0x4000);            /* "nie dziel na kawalki" */
    h[8] = 64;                       /* TTL: po 64 routerach pakiet ginie */
    h[9] = proto;
    put16(h + 10, 0);
    put32(h + 12, info.ip);
    put32(h + 16, dst);
    put16(h + 10, fold(sum_words(h, IP_HDR, 0)));
    eth_send(frame, mac, ETH_IP, IP_HDR + len);
    return 1;
}

static void icmp_input(uint32_t src, unsigned ttl, const uint8_t *p, unsigned len);
static void udp_input(uint32_t src, const uint8_t *p, unsigned len);
static void tcp_input(uint32_t src, const uint8_t *p, unsigned len);

static void ip_input(const uint8_t *h, unsigned len)
{
    if (len < IP_HDR || h[0] >> 4 != 4)
        return;
    unsigned hl = (h[0] & 15) * 4u, total = get16(h + 2);
    if (hl < IP_HDR || total < hl || total > len || fold(sum_words(h, hl, 0)) != 0)
        return;
    if (get16(h + 6) & 0x3FFF)       /* kawalek wiekszego pakietu - nie skladamy ich */
        return;
    uint32_t src = get32(h + 12), dst = get32(h + 16);
    uint32_t bcast = info.ip | ~info.mask;
    if (info.ip && dst != info.ip && dst != 0xFFFFFFFFu && dst != bcast)
        return;                      /* nie do nas */
    const uint8_t *data = h + hl;
    unsigned dlen = total - hl;
    switch (h[9]) {
    case PROTO_ICMP: icmp_input(src, h[8], data, dlen); break;
    case PROTO_UDP:  udp_input(src, data, dlen); break;
    case PROTO_TCP:  tcp_input(src, data, dlen); break;
    }
}

static void net_poll(void)
{
    uint8_t frame[FRAME_MAX + 4];
    unsigned len;
    while ((len = rtl8139_receive(frame, sizeof(frame))) > 0) {
        info.rx_frames++;
        if (len < ETH_HDR)
            continue;
        in_poll++;
        uint16_t type = get16(frame + 12);
        if (type == ETH_ARP)
            arp_input(frame + ETH_HDR, len - ETH_HDR);
        else if (type == ETH_IP)
            ip_input(frame + ETH_HDR, len - ETH_HDR);
        in_poll--;
    }
}

/* ---------- ICMP (ping) ---------- */

#define PING_ID 0x4E53               /* "NS" - rozpoznajemy nasze odpowiedzi */

static struct {
    uint16_t seq;
    int got;
    unsigned ttl;
    uint64_t tsc;
} ping;

static void icmp_input(uint32_t src, unsigned ttl, const uint8_t *p, unsigned len)
{
    if (len < 8 || fold(sum_words(p, len, 0)) != 0)
        return;
    if (p[0] == 8 && len <= IP_MAX_DATA) {           /* ktos nas pinguje - odpowiadamy */
        uint8_t frame[FRAME_MAX];
        uint8_t *r = frame + IP_DATA;
        memcpy(r, p, len);
        r[0] = 0;                                    /* echo reply */
        put16(r + 2, 0);
        put16(r + 2, fold(sum_words(r, len, 0)));
        ip_send(frame, src, PROTO_ICMP, len);
    } else if (p[0] == 0 && get16(p + 4) == PING_ID && get16(p + 6) == ping.seq && !ping.got) {
        ping.tsc = rdtsc();
        ping.ttl = ttl;
        ping.got = 1;
    }
}

static int ping_done(void *arg) { (void)arg; return ping.got; }

int net_ping(uint32_t ip, uint16_t seq, uint32_t timeout_ms, uint32_t *rtt_us, unsigned *ttl)
{
    if (!info.up)
        return NET_EDOWN;
    uint8_t frame[FRAME_MAX];
    uint8_t *p = frame + IP_DATA;
    static const char payload[] = "NaszOS ping - czesc! 0123456789";
    unsigned len = 8 + sizeof(payload);
    p[0] = 8;                        /* echo request */
    p[1] = 0;
    put16(p + 2, 0);
    put16(p + 4, PING_ID);
    put16(p + 6, seq);
    memcpy(p + 8, payload, sizeof(payload));
    put16(p + 2, fold(sum_words(p, len, 0)));

    ping.seq = seq;
    ping.got = 0;
    uint8_t mac[6];                  /* najpierw ARP - zeby nie liczyc go do czasu */
    uint32_t hop = ((ip ^ info.ip) & info.mask) == 0 ? ip : info.gateway;
    if (!arp_resolve(hop, mac))
        return con_cur->closed ? NET_EABORT : NET_ETIMEOUT;
    uint64_t start = rdtsc();
    ip_send(frame, ip, PROTO_ICMP, len);
    if (!wait_for(ping_done, 0, timeout_ms))
        return con_cur->closed ? NET_EABORT : NET_ETIMEOUT;
    *rtt_us = tsc_per_ms ? (uint32_t)((ping.tsc - start) * 1000 / tsc_per_ms) : 0;
    *ttl = ping.ttl;
    return 0;
}

/* ---------- UDP ---------- */

static struct {
    uint16_t port;                   /* na jaki port czekamy (0 = na nic) */
    int got;
    unsigned len;
    uint8_t data[IP_MAX_DATA];
} udp;

static void udp_input(uint32_t src, const uint8_t *p, unsigned len)
{
    (void)src;
    if (len < 8 || get16(p + 4) > len || get16(p + 4) < 8)
        return;
    if (udp.port && get16(p + 2) == udp.port && !udp.got) {
        udp.len = get16(p + 4) - 8u;
        memcpy(udp.data, p + 8, udp.len);
        udp.got = 1;
    }
}

static void udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *data, unsigned len)
{
    uint8_t frame[FRAME_MAX];
    uint8_t *u = frame + IP_DATA;
    put16(u, sport);
    put16(u + 2, dport);
    put16(u + 4, (uint16_t)(8 + len));
    put16(u + 6, 0);                 /* suma kontrolna UDP w IPv4 jest nieobowiazkowa */
    memcpy(u + 8, data, len);
    ip_send(frame, dst, PROTO_UDP, 8 + len);
}

static int udp_arrived(void *arg) { (void)arg; return udp.got; }

/* ---------- DHCP: "dzien dobry, jestem nowy, jaki mam adres?" ---------- */

struct dhcp_reply {
    uint8_t type;
    uint32_t ip, mask, router, dns, server;
};

static unsigned dhcp_build(uint8_t *b, uint8_t type, uint32_t xid, uint32_t want_ip, uint32_t server)
{
    memset(b, 0, 240);
    b[0] = 1;                        /* pytanie */
    b[1] = 1;                        /* Ethernet */
    b[2] = 6;
    put32(b + 4, xid);
    put16(b + 10, 0x8000);           /* odpowiedz rozgloszeniowo - jeszcze nie mamy adresu */
    memcpy(b + 28, info.mac, 6);
    put32(b + 236, 0x63825363);      /* "magiczne ciasteczko" DHCP */
    unsigned n = 240;
    b[n++] = 53; b[n++] = 1; b[n++] = type;
    if (want_ip) {
        b[n++] = 50; b[n++] = 4; put32(b + n, want_ip); n += 4;
    }
    if (server) {
        b[n++] = 54; b[n++] = 4; put32(b + n, server); n += 4;
    }
    b[n++] = 55; b[n++] = 3; b[n++] = 1; b[n++] = 3; b[n++] = 6;   /* prosimy o: maske, router, DNS */
    b[n++] = 255;
    return n;
}

static int dhcp_parse(const uint8_t *b, unsigned len, uint32_t xid, struct dhcp_reply *r)
{
    if (len < 240 || b[0] != 2 || get32(b + 4) != xid || get32(b + 236) != 0x63825363)
        return 0;
    memset(r, 0, sizeof(*r));
    r->ip = get32(b + 16);
    for (unsigned i = 240; i < len && b[i] != 255;) {
        if (b[i] == 0) {
            i++;
            continue;
        }
        if (i + 2 > len || i + 2 + b[i + 1] > len)
            break;
        const uint8_t *v = b + i + 2;
        unsigned l = b[i + 1];
        switch (b[i]) {
        case 53: if (l >= 1) r->type = v[0]; break;
        case 1:  if (l >= 4) r->mask = get32(v); break;
        case 3:  if (l >= 4) r->router = get32(v); break;
        case 6:  if (l >= 4) r->dns = get32(v); break;
        case 54: if (l >= 4) r->server = get32(v); break;
        }
        i += 2 + l;
    }
    return r->type != 0;
}

/* Wysyla wiadomosc DHCP i czeka na odpowiedz typu want. 1 = jest */
static int dhcp_exchange(const uint8_t *msg, unsigned len, uint32_t xid, uint8_t want, struct dhcp_reply *r)
{
    for (int attempt = 0; attempt < 3; attempt++) {
        udp.port = 68;
        udp.got = 0;
        udp_send(0xFFFFFFFFu, 68, 67, msg, len);
        uint32_t start = timer_uptime_ms();
        while (timer_uptime_ms() - start < 1500) {
            if (wait_for(udp_arrived, 0, 1500 - (timer_uptime_ms() - start)) &&
                dhcp_parse(udp.data, udp.len, xid, r) && r->type == want) {
                udp.port = 0;
                return 1;
            }
            udp.got = 0;             /* nie to - czekamy dalej */
        }
    }
    udp.port = 0;
    return 0;
}

static int dhcp(void)
{
    uint8_t msg[300];
    struct dhcp_reply offer, ack;
    uint32_t xid = (uint32_t)rdtsc() ^ 0x4E61737A;
    unsigned len = dhcp_build(msg, 1, xid, 0, 0);                          /* DISCOVER */
    if (!dhcp_exchange(msg, len, xid, 2, &offer))                          /* OFFER */
        return 0;
    len = dhcp_build(msg, 3, xid, offer.ip, offer.server);                 /* REQUEST */
    if (!dhcp_exchange(msg, len, xid, 5, &ack))                            /* ACK */
        return 0;
    info.ip = ack.ip;
    info.mask = ack.mask ? ack.mask : 0xFFFFFF00u;
    info.gateway = ack.router;
    info.dns = ack.dns ? ack.dns : ack.router;
    return 1;
}

/* ---------- DNS: "jaki adres ma google.com?" ---------- */

static unsigned dns_skip_name(const uint8_t *b, unsigned len, unsigned pos)
{
    while (pos < len) {
        uint8_t l = b[pos];
        if (l == 0)
            return pos + 1;
        if ((l & 0xC0) == 0xC0)      /* odnosnik do nazwy wczesniej w wiadomosci */
            return pos + 2;
        pos += l + 1u;
    }
    return len + 1;
}

/* 0 = mamy adres, NET_ENOTFOUND = nie ma takiej nazwy, 1 = bledna odpowiedz */
static int dns_parse(const uint8_t *b, unsigned len, uint16_t id, uint32_t *ip)
{
    if (len < 12 || get16(b) != id || !(b[2] & 0x80))
        return 1;
    if ((b[3] & 15) == 3)
        return NET_ENOTFOUND;
    unsigned questions = get16(b + 4), answers = get16(b + 6), pos = 12;
    for (unsigned i = 0; i < questions; i++)
        pos = dns_skip_name(b, len, pos) + 4;
    for (unsigned i = 0; i < answers && pos < len; i++) {
        pos = dns_skip_name(b, len, pos);
        if (pos + 10 > len)
            break;
        uint16_t type = get16(b + pos), rdlen = get16(b + pos + 8);
        pos += 10;
        if (type == 1 && rdlen == 4 && pos + 4 <= len) {   /* rekord A = adres IPv4 */
            *ip = get32(b + pos);
            return 0;
        }
        pos += rdlen;                                       /* np. CNAME - pomijamy */
    }
    return NET_ENOTFOUND;
}

int net_resolve(const char *name, uint32_t *ip)
{
    if (net_parse_ip(name, ip))
        return 0;
    if (!info.up)
        return NET_EDOWN;

    uint8_t q[300];
    uint16_t id = (uint16_t)rdtsc();
    memset(q, 0, 12);
    put16(q, id);
    put16(q + 2, 0x0100);            /* "szukaj za mnie dalej" (rekursja) */
    put16(q + 4, 1);                 /* jedno pytanie */
    unsigned n = 12;
    /* "www.google.com" -> 3 www 6 google 3 com 0 */
    for (const char *s = name; *s;) {
        const char *dot = s;
        while (*dot && *dot != '.')
            dot++;
        unsigned l = (unsigned)(dot - s);
        if (l == 0 || l > 63 || n + l + 6 > 270)
            return NET_ENOTFOUND;
        q[n++] = (uint8_t)l;
        memcpy(q + n, s, l);
        n += l;
        s = *dot ? dot + 1 : dot;
    }
    q[n++] = 0;
    put16(q + n, 1);                 /* typ A (adres IPv4) */
    put16(q + n + 2, 1);             /* klasa IN (internet) */
    n += 4;

    uint16_t port = (uint16_t)(50000 + id % 10000);
    for (int attempt = 0; attempt < 3; attempt++) {
        udp.port = port;
        udp.got = 0;
        udp_send(info.dns, port, 53, q, n);
        uint32_t start = timer_uptime_ms();
        while (timer_uptime_ms() - start < 2000) {
            if (!wait_for(udp_arrived, 0, 2000 - (timer_uptime_ms() - start)))
                break;
            int r = dns_parse(udp.data, udp.len, id, ip);
            if (r <= 0) {
                udp.port = 0;
                return r;
            }
            udp.got = 0;
        }
        if (con_cur->closed)
            break;
    }
    udp.port = 0;
    return con_cur->closed ? NET_EABORT : NET_ETIMEOUT;
}

/* ---------- NTP: "ktora godzina?" ----------
 * Serwer czasu odpowiada liczba sekund od 1 stycznia 1900 (UTC). */

#define NTP_TO_UNIX 2208988800u      /* sekundy miedzy 1900 a 1970 */

int net_ntp(const char *server, int64_t *unix_secs)
{
    uint32_t ip;
    int err = net_resolve(server, &ip);
    if (err)
        return err;
    uint8_t q[48];
    memset(q, 0, sizeof(q));
    q[0] = 0x23;                     /* wersja 4, tryb 3 = klient */
    uint16_t port = (uint16_t)(51000 + rdtsc() % 1000);
    for (int attempt = 0; attempt < 3; attempt++) {
        udp.port = port;
        udp.got = 0;
        udp_send(ip, port, 123, q, sizeof(q));
        if (wait_for(udp_arrived, 0, 2000) && udp.len >= 48 && (udp.data[0] & 7) == 4) {
            udp.port = 0;
            uint32_t secs = get32(udp.data + 40);   /* "czas wyslania" odpowiedzi */
            *unix_secs = (int64_t)secs - NTP_TO_UNIX;
            return 0;
        }
        if (con_cur->closed)
            break;
    }
    udp.port = 0;
    return con_cur->closed ? NET_EABORT : NET_ETIMEOUT;
}

/* ---------- TCP: niezawodne polaczenie ----------
 * Kazdy bajt ma swoj numer (sequence). Odbiorca potwierdza (ACK), do ktorego
 * bajtu wszystko dostal; czego nie potwierdzi, nadawca wysle jeszcze raz.
 * Nasza wersja jest prosta: wysylamy po jednym kawalku i czekamy na
 * potwierdzenie, a odbieramy tylko po kolei (reszte serwer powtorzy). */

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define RBUF_SIZE  65536
#define WINDOW_MAX 16384             /* tyle bajtow serwer moze wyslac bez czekania */

enum { TCP_CLOSED, TCP_SYN_SENT, TCP_ESTABLISHED };

static struct {
    int state, reset, peer_fin;
    uint32_t ip;
    uint16_t lport, rport, mss, last_wnd;
    uint32_t snd_una, snd_nxt, rcv_nxt;   /* potwierdzone / nastepny do wyslania / oczekiwany */
    unsigned rhead, rlen;                 /* dane odebrane, jeszcze nie przeczytane */
} tcb;
static uint8_t rbuf[RBUF_SIZE];
static uint16_t next_port = 49152;

static uint16_t tcp_window(void)
{
    unsigned space = RBUF_SIZE - tcb.rlen;
    return (uint16_t)(space > WINDOW_MAX ? WINDOW_MAX : space);
}

static uint32_t pseudo_sum(uint32_t src, uint32_t dst, unsigned len)
{
    return (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + PROTO_TCP + len;
}

static void tcp_output(uint8_t flags, uint32_t seq, const void *data, unsigned len)
{
    uint8_t frame[FRAME_MAX];
    uint8_t *t = frame + IP_DATA;
    unsigned hl = (flags & TCP_SYN) ? 24 : 20;
    put16(t, tcb.lport);
    put16(t + 2, tcb.rport);
    put32(t + 4, seq);
    put32(t + 8, (flags & TCP_ACK) ? tcb.rcv_nxt : 0);
    t[12] = (uint8_t)(hl / 4 << 4);
    t[13] = flags;
    tcb.last_wnd = tcp_window();
    put16(t + 14, tcb.last_wnd);
    put16(t + 16, 0);
    put16(t + 18, 0);
    if (flags & TCP_SYN) {           /* opcja MSS: przyjmujemy kawalki do 1460 B */
        t[20] = 2;
        t[21] = 4;
        put16(t + 22, 1460);
    }
    if (len)
        memcpy(t + hl, data, len);
    put16(t + 16, fold(sum_words(t, hl + len, pseudo_sum(info.ip, tcb.ip, hl + len))));
    ip_send(frame, tcb.ip, PROTO_TCP, hl + len);
}

static void tcp_input(uint32_t src, const uint8_t *p, unsigned len)
{
    if (len < 20 || tcb.state == TCP_CLOSED)
        return;
    unsigned hl = (p[12] >> 4) * 4u;
    if (hl < 20 || hl > len || src != tcb.ip || get16(p) != tcb.rport || get16(p + 2) != tcb.lport)
        return;
    if (fold(sum_words(p, len, pseudo_sum(src, info.ip, len))) != 0)
        return;                      /* uszkodzony - serwer wysle ponownie */
    uint32_t seq = get32(p + 4), ack = get32(p + 8);
    uint8_t flags = p[13];

    if (flags & TCP_RST) {
        tcb.reset = 1;
        tcb.state = TCP_CLOSED;
        return;
    }
    if (tcb.state == TCP_SYN_SENT) {
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && ack == tcb.snd_nxt) {
            tcb.rcv_nxt = seq + 1;
            tcb.snd_una = ack;
            for (unsigned i = 20; i + 4 <= hl;) {              /* opcja MSS serwera */
                if (p[i] == 0)
                    break;
                if (p[i] == 1) {
                    i++;
                    continue;
                }
                if (p[i] == 2 && p[i + 1] == 4)
                    tcb.mss = get16(p + i + 2);
                if (p[i + 1] < 2)
                    break;
                i += p[i + 1];
            }
            if (tcb.mss > 1460 || tcb.mss < 64)
                tcb.mss = 536;
            tcb.state = TCP_ESTABLISHED;
            tcp_output(TCP_ACK, tcb.snd_nxt, 0, 0);            /* trojstronne powitanie gotowe */
        }
        return;
    }

    if ((flags & TCP_ACK) && (int32_t)(ack - tcb.snd_una) > 0 && (int32_t)(ack - tcb.snd_nxt) <= 0)
        tcb.snd_una = ack;

    const uint8_t *data = p + hl;
    unsigned dlen = len - hl, taken = 0;
    if (seq == tcb.rcv_nxt && dlen) {                          /* dokladnie to, na co czekamy */
        unsigned space = RBUF_SIZE - tcb.rlen;
        taken = dlen < space ? dlen : space;
        for (unsigned i = 0; i < taken; i++)
            rbuf[(tcb.rhead + tcb.rlen + i) % RBUF_SIZE] = data[i];
        tcb.rlen += taken;
        tcb.rcv_nxt += taken;
    }
    if ((flags & TCP_FIN) && seq == tcb.rcv_nxt - taken && taken == dlen && !tcb.peer_fin) {
        tcb.rcv_nxt++;                                         /* serwer skonczyl wysylac */
        tcb.peer_fin = 1;
    }
    if (dlen || (flags & TCP_FIN))
        tcp_output(TCP_ACK, tcb.snd_nxt, 0, 0);                /* potwierdzamy (albo powtarzamy, na co czekamy) */
}

static int tcp_not_connecting(void *arg) { (void)arg; return tcb.state != TCP_SYN_SENT; }
static int tcp_all_acked(void *arg) { (void)arg; return tcb.snd_una == tcb.snd_nxt || tcb.state != TCP_ESTABLISHED; }
static int tcp_readable(void *arg) { (void)arg; return tcb.rlen || tcb.peer_fin || tcb.state == TCP_CLOSED; }

int tcp_connect(uint32_t ip, uint16_t port)
{
    if (!info.up)
        return NET_EDOWN;
    memset(&tcb, 0, sizeof(tcb));
    tcb.ip = ip;
    tcb.rport = port;
    tcb.lport = next_port++;
    if (next_port > 65000)
        next_port = 49152;
    tcb.mss = 536;
    uint32_t iss = (uint32_t)rdtsc();                          /* numer startowy - losowy */
    tcb.snd_una = iss;
    tcb.snd_nxt = iss + 1;
    tcb.state = TCP_SYN_SENT;
    for (int attempt = 0; attempt < 4 && tcb.state == TCP_SYN_SENT; attempt++) {
        tcp_output(TCP_SYN, iss, 0, 0);                        /* "czesc, chce pogadac" */
        if (wait_for(tcp_not_connecting, 0, 1500) || con_cur->closed)
            break;
    }
    if (tcb.state == TCP_ESTABLISHED)
        return 0;
    int reset = tcb.reset;
    tcb.state = TCP_CLOSED;
    return reset ? NET_ERESET : con_cur->closed ? NET_EABORT : NET_ETIMEOUT;
}

int tcp_send(const void *data, unsigned len)
{
    const uint8_t *d = data;
    while (len) {
        if (tcb.state != TCP_ESTABLISHED)
            return tcb.reset ? NET_ERESET : NET_EDOWN;
        unsigned n = len < tcb.mss ? len : tcb.mss;
        uint32_t seq = tcb.snd_nxt;
        tcb.snd_nxt += n;
        int acked = 0;
        for (int attempt = 0; attempt < 5 && !acked; attempt++) {
            tcp_output(TCP_ACK | TCP_PSH, seq, d, n);
            acked = wait_for(tcp_all_acked, 0, 1000) && tcb.state == TCP_ESTABLISHED;
            if (tcb.reset || con_cur->closed)
                break;
        }
        if (!acked)
            return tcb.reset ? NET_ERESET : con_cur->closed ? NET_EABORT : NET_ETIMEOUT;
        d += n;
        len -= n;
    }
    return 0;
}

int tcp_recv(void *buf, unsigned size, uint32_t timeout_ms)
{
    wait_for(tcp_readable, 0, timeout_ms);
    if (tcb.rlen) {
        unsigned n = size < tcb.rlen ? size : tcb.rlen;
        uint8_t *b = buf;
        for (unsigned i = 0; i < n; i++)
            b[i] = rbuf[(tcb.rhead + i) % RBUF_SIZE];
        tcb.rhead = (tcb.rhead + n) % RBUF_SIZE;
        tcb.rlen -= n;
        /* Zrobilo sie miejsce - dajemy znac serwerowi, zeby slal dalej */
        if (tcb.state == TCP_ESTABLISHED && tcb.last_wnd < WINDOW_MAX / 2 && tcp_window() >= WINDOW_MAX / 2)
            tcp_output(TCP_ACK, tcb.snd_nxt, 0, 0);
        return (int)n;
    }
    if (tcb.peer_fin)
        return 0;
    if (tcb.reset)
        return NET_ERESET;
    return con_cur->closed ? NET_EABORT : NET_ETIMEOUT;
}

void tcp_close(void)
{
    if (tcb.state == TCP_ESTABLISHED) {
        tcp_output(TCP_FIN | TCP_ACK, tcb.snd_nxt, 0, 0);      /* "dziekuje, koncze" */
        tcb.snd_nxt++;
        wait_for(tcp_all_acked, 0, 1000);
    }
    tcb.state = TCP_CLOSED;
}

/* ---------- start ---------- */

int net_init(void)
{
    if (!rtl8139_init(info.mac))
        return 0;
    info.card = 1;

    /* Ile taktow procesora trwa milisekunda - do mierzenia czasu pingu */
    uint64_t t0 = rdtsc();
    timer_sleep(50);
    tsc_per_ms = (rdtsc() - t0) / 50;

    net_lock();
    info.dhcp = dhcp();
    if (!info.dhcp) {                /* bez DHCP: typowe ustawienia sieci QEMU */
        info.ip = 0x0A00020F;        /* 10.0.2.15 */
        info.mask = 0xFFFFFF00u;
        info.gateway = 0x0A000202;   /* 10.0.2.2 */
        info.dns = 0x0A000203;       /* 10.0.2.3 */
    }
    info.up = 1;
    net_unlock();
    return 1;
}

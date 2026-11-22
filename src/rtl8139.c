/* Sterownik RTL8139 - patrz rtl8139.h.
 *
 * Odbior: karta sama (DMA) wpisuje odebrane ramki do naszego bufora
 * pierscieniowego (rx_ring) i zglasza przerwanie. Obsluga przerwania
 * przepisuje ramki do kolejki programowej (rx_queue), skad net.c zabiera je
 * w spokojnym momencie - dzieki temu pierscien karty sie nie przepelnia.
 * Wysylanie: karta ma 4 "sloty" nadawcze - wpisujemy adres ramki i dlugosc,
 * a karta sama ja wysyla i ustawia bit "gotowe". */

#include "crypto.h"
#include "rtl8139.h"
#include "pci.h"
#include "io.h"
#include "idt.h"
#include "string.h"

/* Rejestry karty (przesuniecia od adresu portow z BAR0) */
#define REG_MAC      0x00
#define REG_TSD0     0x10     /* status slotu nadawczego 0..3 (co 4 bajty) */
#define REG_TSAD0    0x20     /* adres ramki w slocie 0..3 */
#define REG_RBSTART  0x30     /* adres bufora odbiorczego */
#define REG_CR       0x37     /* polecenia */
#define REG_CAPR     0x38     /* dokad juz przeczytalismy bufor */
#define REG_IMR      0x3C     /* ktore zdarzenia daja przerwanie */
#define REG_ISR      0x3E     /* ktore zdarzenia zaszly */
#define REG_RCR      0x44     /* co odbierac */
#define REG_CONFIG1  0x52

#define CR_BUFE      0x01     /* bufor odbiorczy pusty */
#define CR_TE        0x04     /* nadajnik wlaczony */
#define CR_RE        0x08     /* odbiornik wlaczony */
#define CR_RST       0x10     /* reset */

#define INT_ROK      0x0001   /* odebrano ramke */
#define INT_RER      0x0002   /* blad odbioru */
#define INT_TOK      0x0004   /* wyslano ramke */
#define INT_TER      0x0008   /* blad wysylania */
#define INT_RXOVW    0x0010   /* bufor odbiorczy przepelniony */
#define INT_FOVW     0x0040   /* kolejka FIFO odbiornika przepelniona */

#define TSD_OWN      0x2000   /* karta skonczyla przepisywac ramke */

#define RX_LEN       32768    /* pierscien 32 KiB (+16 B zapasu +1536 na zawijanie) */
#define TX_SLOTS     4
#define FRAME_MAX    1536

#define QUEUE_LEN    32

static uint16_t io;
static uint8_t rx_ring[RX_LEN + 16 + FRAME_MAX] __attribute__((aligned(16)));
static uint8_t tx_buf[TX_SLOTS][FRAME_MAX] __attribute__((aligned(16)));
static unsigned rx_off, tx_next;
static int tx_used[TX_SLOTS];

static struct {
    uint16_t len;
    uint8_t data[FRAME_MAX];
} rx_queue[QUEUE_LEN];
static volatile unsigned q_head, q_tail;      /* head: czytamy, tail: dopisujemy */

static void rx_start(void)
{
    outb(io + REG_CR, CR_TE);                  /* odbiornik stop - kasuje wskazniki */
    rx_off = 0;
    outl(io + REG_RBSTART, (uint32_t)(uintptr_t)rx_ring);
    /* Przyjmuj ramki: do nas (APM), multicast (AM), rozgloszeniowe (AB);
     * WRAP: ramka moze wyjsc za koniec pierscienia (mamy zapas);
     * RBLEN=2: pierscien 32 KiB; MXDMA=7 i RXFTH=7: bez limitow DMA */
    outl(io + REG_RCR, 0x0E | 0x80 | 2 << 11 | 7 << 8 | 7 << 13);
    outb(io + REG_CR, CR_RE | CR_TE);
}

/* Przepisuje ramki z pierscienia karty do kolejki (z przerwania) */
static void drain_ring(void)
{
    while (!(inb(io + REG_CR) & CR_BUFE)) {
        const uint8_t *p = rx_ring + rx_off;
        uint16_t status = (uint16_t)(p[0] | p[1] << 8);
        uint16_t len = (uint16_t)(p[2] | p[3] << 8);        /* z 4 bajtami CRC na koncu */
        if (!(status & 1) || len < 64 || len > 1518) {      /* bledny naglowek - od nowa */
            rx_start();
            return;
        }
        unsigned next = (q_tail + 1) % QUEUE_LEN;
        if (next != q_head) {                                /* pelna kolejka - ramka przepada */
            rx_queue[q_tail].len = (uint16_t)(len - 4);
            memcpy(rx_queue[q_tail].data, p + 4, len - 4u);
            q_tail = next;
        }
        rx_off = (rx_off + len + 4 + 3) & ~3u;
        rx_off %= RX_LEN;
        outw(io + REG_CAPR, (uint16_t)(rx_off - 16));       /* karta liczy z przesunieciem 16 */
    }
}

static void rtl_irq(struct regs *r)
{
    (void)r;
    uint16_t isr = inw(io + REG_ISR);
    if (!isr)
        return;                              /* to nie my (wspoldzielone IRQ) */
    random_mix(isr);                         /* czas przyjscia pakietu - losowosc dla TLS */
    outw(io + REG_ISR, isr);                   /* potwierdzamy (zapis jedynek kasuje bity) */
    if (isr & (INT_RXOVW | INT_FOVW | INT_RER))
        rx_start();
    else if (isr & INT_ROK)
        drain_ring();
}

int rtl8139_init(uint8_t mac[6])
{
    struct pci_device d;
    if (!pci_find(0x10EC, 0x8139, &d))
        return 0;
    /* Porty + DMA: karta ma sama zapisywac odebrane ramki do RAM */
    pci_write(&d, PCI_COMMAND, pci_read(&d, PCI_COMMAND) | PCI_CMD_IO | PCI_CMD_MASTER);
    io = (uint16_t)(pci_read(&d, PCI_BAR0) & ~3u);
    unsigned irq = pci_read(&d, PCI_INTERRUPT) & 0xFF;

    outb(io + REG_CONFIG1, 0);                 /* wybudzenie */
    outb(io + REG_CR, CR_RST);
    for (int i = 0; i < 1000000 && (inb(io + REG_CR) & CR_RST); i++)
        ;
    for (int i = 0; i < 6; i++)
        mac[i] = inb(io + REG_MAC + i);

    rx_start();
    for (int i = 0; i < TX_SLOTS; i++)
        outl(io + REG_TSAD0 + 4 * i, (uint32_t)(uintptr_t)tx_buf[i]);

    if (irq < 16) {
        interrupt_register((uint8_t)(IRQ_BASE + irq), rtl_irq);
        irq_unmask((uint8_t)irq);
    }
    outw(io + REG_IMR, INT_ROK | INT_RER | INT_TOK | INT_TER | INT_RXOVW | INT_FOVW);
    return 1;
}

int rtl8139_send(const void *frame, unsigned len)
{
    if (!io || len > FRAME_MAX - 4)
        return 0;
    unsigned slot = tx_next;
    if (tx_used[slot]) {                       /* poczekaj, az karta skonczy poprzednia ramke */
        int i = 0;
        while (!(inl(io + REG_TSD0 + 4 * slot) & TSD_OWN))
            if (++i > 1000000)
                return 0;
    }
    memcpy(tx_buf[slot], frame, len);
    if (len < 60) {                            /* Ethernet: ramka ma co najmniej 60 B */
        memset(tx_buf[slot] + len, 0, 60 - len);
        len = 60;
    }
    tx_used[slot] = 1;
    outl(io + REG_TSD0 + 4 * slot, len);       /* OWN=0 + dlugosc = "wyslij" */
    tx_next = (slot + 1) % TX_SLOTS;
    return 1;
}

unsigned rtl8139_receive(void *buf, unsigned size)
{
    unsigned long flags = irq_save();
    unsigned len = 0;
    if (q_head != q_tail) {
        len = rx_queue[q_head].len;
        if (len > size)
            len = size;
        memcpy(buf, rx_queue[q_head].data, len);
        q_head = (q_head + 1) % QUEUE_LEN;
    }
    irq_restore(flags);
    return len;
}

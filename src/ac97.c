/* Sterownik AC'97 - patrz ac97.h.
 *
 * Karta ma dwa zestawy rejestrow (porty z BAR0 i BAR1):
 *   NAM  - "mikser": glosnosc, czestotliwosc probkowania;
 *   NABM - "bus master": karta sama czyta probki z RAM (DMA).
 * Probki podajemy przez liste opisow buforow (BDL, do 32 wpisow): kazdy
 * wpis to adres i dlugosc kawalka dzwieku. Karta gra je po kolei w kolko,
 * az do wpisu nr LVI ("ostatni wazny"). Po kazdym kawalku zglasza
 * przerwanie - wtedy dopisujemy nastepny i przesuwamy LVI, wiec muzyka
 * plynie bez przerwy, a my trzymamy tylko kilka kawalkow "na zapas". */

#include "ac97.h"
#include "pci.h"
#include "io.h"
#include "idt.h"
#include "string.h"

/* NAM */
#define NAM_RESET         0x00
#define NAM_MASTER_VOLUME 0x02
#define NAM_PCM_VOLUME    0x18

/* NABM - kanal wyjscia PCM */
#define PO_BDBAR  0x10       /* adres listy buforow */
#define PO_CIV    0x14       /* ktory bufor gra teraz */
#define PO_LVI    0x15       /* ostatni wazny bufor */
#define PO_SR     0x16       /* status */
#define PO_CR     0x1B       /* sterowanie */
#define GLOB_CNT  0x2C

#define CR_RUN    0x01
#define CR_RESET  0x02
#define CR_IOCE   0x10       /* przerwanie po kazdym buforze */

#define SR_DCH    0x01       /* DMA zatrzymane */
#define SR_CLEAR  0x1C       /* LVBCI | BCIS | FIFOE - kasowane zapisem jedynek */

#define BUFFERS   32
#define FRAMES    512        /* probek stereo w buforze = ok. 10,7 ms */
#define AHEAD     4          /* tyle buforow czeka w kolejce (ok. 43 ms) */

struct bd {
    uint32_t addr;
    uint16_t samples;        /* liczba 16-bitowych probek (lewa + prawa osobno) */
    uint16_t flags;
} __attribute__((packed));

#define BD_IOC 0x8000

static struct bd bdl[BUFFERS] __attribute__((aligned(16)));
static int16_t buffers[BUFFERS][FRAMES * 2] __attribute__((aligned(16)));
static uint16_t nam, nabm;
static unsigned last_filled;
static ac97_fill_fn fill_fn;

static void fill(unsigned i)
{
    fill_fn(buffers[i], FRAMES);
}

/* Dolewa buforow, zeby przed tym grajacym zawsze czekalo AHEAD kolejnych */
static void top_up(void)
{
    unsigned civ = inb(nabm + PO_CIV) % BUFFERS;
    while ((last_filled + BUFFERS - civ) % BUFFERS < AHEAD) {
        last_filled = (last_filled + 1) % BUFFERS;
        fill(last_filled);
    }
    outb(nabm + PO_LVI, (uint8_t)last_filled);
}

static void ac97_irq(struct regs *r)
{
    (void)r;
    uint16_t sr = inw(nabm + PO_SR);
    if (!(sr & SR_CLEAR))
        return;                              /* to nie my (wspoldzielone IRQ) */
    outw(nabm + PO_SR, sr & SR_CLEAR);
    top_up();
    if (sr & SR_DCH)                         /* karta stanela (za pozno dolalismy) - ruszamy */
        outb(nabm + PO_CR, CR_RUN | CR_IOCE);
}

int ac97_init(ac97_fill_fn fn)
{
    struct pci_device d;
    if (!pci_find(0x8086, 0x2415, &d))
        return 0;
    pci_write(&d, PCI_COMMAND, pci_read(&d, PCI_COMMAND) | PCI_CMD_IO | PCI_CMD_MASTER);
    nam = (uint16_t)(pci_read(&d, PCI_BAR0) & ~3u);
    nabm = (uint16_t)(pci_read(&d, PCI_BAR0 + 4) & ~3u);
    unsigned irq = pci_read(&d, PCI_INTERRUPT) & 0xFF;
    fill_fn = fn;

    outl(nabm + GLOB_CNT, 0x02);             /* koniec "zimnego resetu" ukladu dzwiekowego */
    for (volatile int i = 0; i < 100000; i++)
        ;
    outw(nam + NAM_RESET, 0);
    outw(nam + NAM_MASTER_VOLUME, 0x0000);   /* 0 = najglosniej, bez wyciszenia */
    outw(nam + NAM_PCM_VOLUME, 0x0808);      /* 0 dB */

    outb(nabm + PO_CR, CR_RESET);            /* reset kanalu wyjscia */
    for (int i = 0; i < 100000 && (inb(nabm + PO_CR) & CR_RESET); i++)
        ;
    for (int i = 0; i < BUFFERS; i++) {
        bdl[i].addr = (uint32_t)(uintptr_t)buffers[i];
        bdl[i].samples = FRAMES * 2;
        bdl[i].flags = BD_IOC;
    }
    for (last_filled = 0; last_filled < AHEAD; last_filled++)
        fill(last_filled);
    last_filled = AHEAD - 1;
    outl(nabm + PO_BDBAR, (uint32_t)(uintptr_t)bdl);
    outb(nabm + PO_LVI, (uint8_t)last_filled);

    if (irq < 16) {
        interrupt_register((uint8_t)(IRQ_BASE + irq), ac97_irq);
        irq_unmask((uint8_t)irq);
    }
    outb(nabm + PO_CR, CR_RUN | CR_IOCE);    /* graj! */
    return 1;
}

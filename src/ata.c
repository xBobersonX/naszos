/* Sterownik dysku ATA (IDE) w trybie PIO - procesor sam przepisuje dane
 * slowo po slowie przez port 0x1F0. Obslugujemy pierwszy dysk na pierwszym
 * kanale (primary master), adresowanie LBA28 (do 128 GiB). Nie uzywamy
 * przerwan - czekamy, odpytujac rejestr statusu. */

#include "ata.h"
#include "io.h"

#define ATA_DATA      0x1F0
#define ATA_ERROR     0x1F1
#define ATA_COUNT     0x1F2
#define ATA_LBA_LO    0x1F3
#define ATA_LBA_MID   0x1F4
#define ATA_LBA_HI    0x1F5
#define ATA_DRIVE     0x1F6
#define ATA_STATUS    0x1F7   /* odczyt */
#define ATA_COMMAND   0x1F7   /* zapis */
#define ATA_CONTROL   0x3F6   /* zapis; odczyt = status bez kasowania przerwania */

#define SR_BSY  0x80          /* dysk zajety */
#define SR_DF   0x20          /* awaria urzadzenia */
#define SR_DRQ  0x08          /* dysk gotowy na przeslanie danych */
#define SR_ERR  0x01

#define CMD_READ     0x20
#define CMD_WRITE    0x30
#define CMD_FLUSH    0xE7
#define CMD_IDENTIFY 0xEC

#define TIMEOUT 1000000       /* liczba odczytow statusu, zanim uznamy, ze dysk nie odpowiada */
#define MAX_CHUNK 128         /* sektorow na jedno polecenie */

static uint32_t total_sectors;
static char model[41];

static void delay_400ns(void)
{
    for (int i = 0; i < 4; i++)
        inb(ATA_CONTROL);
}

static int wait_not_busy(void)
{
    for (uint32_t i = 0; i < TIMEOUT; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (!(s & SR_BSY))
            return (s & (SR_ERR | SR_DF)) ? -1 : 0;
    }
    return -1;
}

static int wait_drq(void)
{
    for (uint32_t i = 0; i < TIMEOUT; i++) {
        uint8_t s = inb(ATA_STATUS);
        if (s & SR_BSY)
            continue;
        if (s & (SR_ERR | SR_DF))
            return -1;
        if (s & SR_DRQ)
            return 0;
    }
    return -1;
}

int ata_init(void)
{
    outb(ATA_CONTROL, 0x02);            /* wylaczamy przerwania od dysku (nIEN) */
    outb(ATA_DRIVE, 0xA0);              /* wybor: master */
    delay_400ns();
    outb(ATA_COUNT, 0);
    outb(ATA_LBA_LO, 0);
    outb(ATA_LBA_MID, 0);
    outb(ATA_LBA_HI, 0);
    outb(ATA_COMMAND, CMD_IDENTIFY);

    uint8_t s = inb(ATA_STATUS);
    if (s == 0 || s == 0xFF)            /* brak dysku albo brak kontrolera */
        return 0;
    for (uint32_t i = 0; i < TIMEOUT && (inb(ATA_STATUS) & SR_BSY); i++)
        ;
    if (inb(ATA_LBA_MID) || inb(ATA_LBA_HI))   /* to nie dysk ATA (np. naped CD) */
        return 0;
    if (wait_drq() < 0)
        return 0;

    uint16_t id[256];
    for (int i = 0; i < 256; i++)
        id[i] = inw(ATA_DATA);

    total_sectors = id[60] | (uint32_t)id[61] << 16;
    for (int i = 0; i < 20; i++) {      /* nazwa modelu: znaki zamienione parami */
        model[i * 2]     = (char)(id[27 + i] >> 8);
        model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    for (int i = 39; i >= 0 && model[i] == ' '; i--)
        model[i] = '\0';
    return total_sectors > 0;
}

uint32_t ata_sectors(void)   { return total_sectors; }
const char *ata_model(void)  { return model; }

static void select_lba(uint32_t lba, uint32_t count)
{
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));   /* master + tryb LBA + bity 24-27 */
    delay_400ns();
    outb(ATA_COUNT, (uint8_t)count);
    outb(ATA_LBA_LO, (uint8_t)lba);
    outb(ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(ATA_LBA_HI, (uint8_t)(lba >> 16));
}

static int check_range(uint32_t lba, uint32_t count)
{
    return total_sectors && count && lba < total_sectors && count <= total_sectors - lba;
}

int ata_read(uint32_t lba, uint32_t count, void *buf)
{
    if (!check_range(lba, count))
        return -1;
    uint16_t *p = buf;
    while (count) {
        uint32_t n = count > MAX_CHUNK ? MAX_CHUNK : count;
        if (wait_not_busy() < 0)
            return -1;
        select_lba(lba, n);
        outb(ATA_COMMAND, CMD_READ);
        for (uint32_t s = 0; s < n; s++) {
            if (wait_drq() < 0)
                return -1;
            for (int i = 0; i < 256; i++)
                *p++ = inw(ATA_DATA);
        }
        lba += n;
        count -= n;
    }
    return 0;
}

int ata_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (!check_range(lba, count))
        return -1;
    const uint16_t *p = buf;
    while (count) {
        uint32_t n = count > MAX_CHUNK ? MAX_CHUNK : count;
        if (wait_not_busy() < 0)
            return -1;
        select_lba(lba, n);
        outb(ATA_COMMAND, CMD_WRITE);
        for (uint32_t s = 0; s < n; s++) {
            if (wait_drq() < 0)
                return -1;
            for (int i = 0; i < 256; i++)
                outw(ATA_DATA, *p++);
        }
        outb(ATA_COMMAND, CMD_FLUSH);   /* wymuszamy zapis z pamieci podrecznej dysku */
        if (wait_not_busy() < 0)
            return -1;
        lba += n;
        count -= n;
    }
    return 0;
}

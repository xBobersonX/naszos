/* Trwaly zapis systemu plikow na dysku.
 *
 * Cale drzewo plikow zapisujemy jako jedna "migawke". Dysk jest podzielony
 * na dwie polowki (sloty) i kolejne zapisy trafiaja na zmiane do jednej
 * i drugiej. Kazdy slot to:
 *
 *   sektor 0      naglowek: "NASZOSFS", numer generacji, dlugosc danych, CRC32
 *   sektory 1..   dane: rekordy {typ, dlugosc sciezki, dlugosc tresci, sciezka, tresc}
 *
 * Najpierw zapisujemy dane, a naglowek na samym koncu. Jesli zapis sie
 * przerwie (np. zabraknie pradu), nowy naglowek nie powstanie albo CRC sie
 * nie zgodzi - wtedy przy starcie wczytamy poprzednia, cala migawke z drugiego
 * slotu. Przy wczytywaniu wybieramy poprawny slot z najwyzsza generacja. */

#include "storage.h"
#include "ata.h"
#include "fs.h"
#include "heap.h"
#include "string.h"
#include "vga.h"

#define MAGIC     "NASZOSFS"
#define VERSION   1
#define REC_DIR   1
#define REC_FILE  2

struct header {
    char     magic[8];
    uint32_t version;
    uint32_t generation;      /* rosnie z kazdym zapisem */
    uint32_t payload_len;     /* bajty danych za naglowkiem */
    uint32_t payload_crc;
    uint32_t entries;         /* liczba plikow i katalogow */
    uint32_t header_crc;      /* CRC powyzszych pol */
} __attribute__((packed));

struct record {
    uint8_t  type;
    uint8_t  reserved;
    uint16_t path_len;
    uint32_t data_len;
} __attribute__((packed));

static int disk_present;
static uint32_t slot_sectors;
static int active_slot = -1;          /* slot z ostatnia poprawna migawka */
static uint32_t generation;
static uint32_t last_payload, last_entries;

static const char *const errors[] = {
    "OK", "brak zapisanego systemu plikow", "brak dysku", "blad odczytu/zapisu dysku",
    "brak miejsca na dysku", "brak pamieci", "dane na dysku sa uszkodzone",
};

const char *storage_strerror(int err)
{
    if (err > 0 || -err >= (int)(sizeof(errors) / sizeof(errors[0])))
        return "nieznany blad";
    return errors[-err];
}

/* CRC32 (ten sam co w ZIP/PNG) - wykrywa uszkodzone lub niedokonczone dane */
static uint32_t crc32(const void *data, uint32_t len)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFF;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320 & (0u - (crc & 1)));
    }
    return ~crc;
}

static uint32_t slot_lba(int slot)
{
    return (uint32_t)slot * slot_sectors;
}

int storage_init(void)
{
    disk_present = ata_init();
    if (disk_present)
        slot_sectors = ata_sectors() / 2;
    if (slot_sectors < 2)             /* za maly dysk na dwa sloty */
        disk_present = 0;
    return disk_present;
}

/* ---------- zapis ---------- */

struct writer {
    uint8_t *buf;                     /* NULL = tylko liczymy rozmiar */
    uint32_t pos;
    uint32_t entries;
};

static void put(struct writer *w, const void *data, uint32_t n)
{
    if (w->buf)
        memcpy(w->buf + w->pos, data, n);
    w->pos += n;
}

/* Zapisuje zawartosc katalogu; rodzic zawsze przed dziecmi, zeby przy
 * wczytywaniu katalog istnial, zanim tworzymy w nim pliki */
static void serialize(struct writer *w, const struct fs_node *dir, char *path, uint32_t len)
{
    for (const struct fs_node *c = dir->children; c; c = c->next) {
        uint32_t nl = (uint32_t)strlen(c->name);
        path[len] = '/';
        memcpy(path + len + 1, c->name, nl);
        uint32_t plen = len + 1 + nl;

        struct record r = {
            .type = c->is_dir ? REC_DIR : REC_FILE,
            .path_len = (uint16_t)plen,
            .data_len = c->is_dir ? 0 : c->size,
        };
        put(w, &r, sizeof(r));
        put(w, path, plen);
        if (!c->is_dir)
            put(w, c->data, c->size);
        w->entries++;

        if (c->is_dir)
            serialize(w, c, path, plen);
    }
}

int storage_save(void)
{
    if (!disk_present)
        return STORAGE_ENODISK;

    char path[FS_PATH_MAX];
    struct writer w = { 0 };
    serialize(&w, fs_root(), path, 0);            /* 1. przebieg: liczymy rozmiar */
    uint32_t payload = w.pos;

    uint32_t data_sectors = (payload + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
    if (1 + data_sectors > slot_sectors)
        return STORAGE_ENOSPC;

    uint8_t *buf = kzalloc((1 + data_sectors) * ATA_SECTOR_SIZE);
    if (!buf)
        return STORAGE_ENOMEM;
    w.buf = buf + ATA_SECTOR_SIZE;
    w.pos = w.entries = 0;
    serialize(&w, fs_root(), path, 0);            /* 2. przebieg: zapisujemy do bufora */

    struct header *h = (struct header *)buf;
    memcpy(h->magic, MAGIC, 8);
    h->version     = VERSION;
    h->generation  = generation + 1;
    h->payload_len = payload;
    h->payload_crc = crc32(w.buf, payload);
    h->entries     = w.entries;
    h->header_crc  = crc32(h, sizeof(*h) - sizeof(h->header_crc));

    int slot = active_slot == 0 ? 1 : 0;          /* piszemy do slotu, ktorego NIE uzywamy */
    uint32_t lba = slot_lba(slot);
    int err = STORAGE_OK;
    if (data_sectors && ata_write(lba + 1, data_sectors, w.buf) < 0)
        err = STORAGE_EIO;
    else if (ata_write(lba, 1, buf) < 0)          /* naglowek na samym koncu */
        err = STORAGE_EIO;
    kfree(buf);
    if (err)
        return err;

    active_slot = slot;
    generation++;
    last_payload = payload;
    last_entries = w.entries;
    fs_clear_dirty();
    return STORAGE_OK;
}

int storage_sync_if_dirty(void)
{
    if (!disk_present || !fs_is_dirty())
        return STORAGE_OK;
    return storage_save();
}

/* ---------- odczyt ---------- */

static int read_header(int slot, struct header *out)
{
    uint8_t sector[ATA_SECTOR_SIZE];
    if (ata_read(slot_lba(slot), 1, sector) < 0)
        return STORAGE_EIO;
    memcpy(out, sector, sizeof(*out));
    if (memcmp(out->magic, MAGIC, 8) != 0)
        return STORAGE_EMPTY;
    if (out->version != VERSION ||
        out->header_crc != crc32(out, sizeof(*out) - sizeof(out->header_crc)) ||
        out->payload_len > (slot_sectors - 1) * ATA_SECTOR_SIZE)
        return STORAGE_ECORRUPT;
    return STORAGE_OK;
}

/* Odtwarza drzewo z danych migawki */
static int apply(const uint8_t *p, uint32_t len)
{
    uint32_t pos = 0;
    char path[FS_PATH_MAX];

    while (pos < len) {
        struct record r;
        if (len - pos < sizeof(r))
            return STORAGE_ECORRUPT;
        memcpy(&r, p + pos, sizeof(r));
        pos += sizeof(r);
        if (r.path_len == 0 || r.path_len >= FS_PATH_MAX ||
            r.path_len > len - pos || r.data_len > len - pos - r.path_len)
            return STORAGE_ECORRUPT;
        memcpy(path, p + pos, r.path_len);
        path[r.path_len] = '\0';
        pos += r.path_len;

        struct fs_node *n;
        if (fs_create(fs_root(), path, r.type == REC_DIR, &n) != FS_OK)
            return STORAGE_ECORRUPT;
        if (r.type == REC_FILE && r.data_len && fs_write(n, (const char *)p + pos, r.data_len, 0) != FS_OK)
            return STORAGE_ENOMEM;
        pos += r.data_len;
    }
    return STORAGE_OK;
}

int storage_load(uint32_t *entries)
{
    if (!disk_present)
        return STORAGE_ENODISK;

    struct header h[2];
    int st[2] = { read_header(0, &h[0]), read_header(1, &h[1]) };
    if (st[0] == STORAGE_EIO && st[1] == STORAGE_EIO)
        return STORAGE_EIO;
    if (st[0] == STORAGE_EMPTY && st[1] == STORAGE_EMPTY)
        return STORAGE_EMPTY;

    /* Najpierw probujemy nowsza migawke, potem starsza */
    int order[2] = { 0, 1 };
    if (st[1] == STORAGE_OK && (st[0] != STORAGE_OK || h[1].generation > h[0].generation)) {
        order[0] = 1;
        order[1] = 0;
    }

    int result = STORAGE_ECORRUPT;
    for (int i = 0; i < 2; i++) {
        int slot = order[i];
        if (st[slot] != STORAGE_OK)
            continue;
        uint32_t sectors = (h[slot].payload_len + ATA_SECTOR_SIZE - 1) / ATA_SECTOR_SIZE;
        uint8_t *buf = sectors ? kmalloc(sectors * ATA_SECTOR_SIZE) : 0;
        if (sectors && !buf)
            return STORAGE_ENOMEM;
        if (sectors && ata_read(slot_lba(slot) + 1, sectors, buf) < 0) {
            kfree(buf);
            result = STORAGE_EIO;
            continue;
        }
        if (crc32(buf, h[slot].payload_len) != h[slot].payload_crc) {
            kfree(buf);
            continue;                 /* niedokonczony zapis - probujemy drugi slot */
        }
        result = apply(buf, h[slot].payload_len);
        kfree(buf);
        if (result != STORAGE_OK)
            return result;

        active_slot = slot;
        generation = h[slot].generation;
        last_payload = h[slot].payload_len;
        last_entries = h[slot].entries;
        *entries = h[slot].entries;
        fs_clear_dirty();
        return STORAGE_OK;
    }
    return result;
}

void storage_print_info(void)
{
    if (!disk_present) {
        vga_write("Brak dysku - pliki sa tylko w pamieci RAM i znikna po wylaczeniu.\n");
        return;
    }
    uint32_t capacity = (slot_sectors - 1) * ATA_SECTOR_SIZE;
    kprintf("Dysk:        %s\n", ata_model());
    kprintf("Pojemnosc:   %u KiB (%u sektorow po 512 B)\n", ata_sectors() / 2, ata_sectors());
    kprintf("Miejsce:     %u B z %u B zajete (%u%%)\n",
            last_payload, capacity, last_payload / (capacity / 100 ? capacity / 100 : 1));
    kprintf("Zapisane:    %u plikow i katalogow\n", last_entries);
    if (active_slot >= 0)
        kprintf("Migawka:     nr %u w slocie %c\n", generation, active_slot ? 'B' : 'A');
    kprintf("Stan:        %s\n", fs_is_dirty() ? "sa niezapisane zmiany" : "wszystko zapisane");
}

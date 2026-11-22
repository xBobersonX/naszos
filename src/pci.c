/* Dostep do przestrzeni konfiguracyjnej PCI przez porty 0xCF8 (adres)
 * i 0xCFC (dane) - "mechanizm nr 1", dzialajacy na kazdym PC. */

#include "pci.h"
#include "io.h"

static uint32_t address(const struct pci_device *d, unsigned offset)
{
    return 0x80000000u | d->bus << 16 | d->dev << 11 | d->func << 8 | (offset & 0xFC);
}

uint32_t pci_read(const struct pci_device *d, unsigned offset)
{
    outl(0xCF8, address(d, offset));
    return inl(0xCFC);
}

void pci_write(const struct pci_device *d, unsigned offset, uint32_t value)
{
    outl(0xCF8, address(d, offset));
    outl(0xCFC, value);
}

int pci_find(uint16_t vendor, uint16_t device, struct pci_device *out)
{
    uint32_t want = (uint32_t)device << 16 | vendor;
    for (unsigned bus = 0; bus < 8; bus++)
        for (unsigned dev = 0; dev < 32; dev++)
            for (unsigned func = 0; func < 8; func++) {
                struct pci_device d = { bus, dev, func };
                uint32_t id = pci_read(&d, PCI_VENDOR_ID);
                if (id == want) {
                    *out = d;
                    return 1;
                }
                if (func == 0 && (id & 0xFFFF) == 0xFFFF)
                    break;          /* nie ma tu urzadzenia - nie sprawdzamy funkcji 1-7 */
            }
    return 0;
}

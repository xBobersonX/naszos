#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/* Szyna PCI - tu siedza karty rozszerzen (grafika, siec, dysk...). Kazde
 * urzadzenie ma 256 bajtow "przestrzeni konfiguracyjnej": kto je zrobil
 * (vendor), co to jest (device), gdzie ma rejestry (BAR) i ktore IRQ. */

struct pci_device {
    unsigned bus, dev, func;
};

uint32_t pci_read(const struct pci_device *d, unsigned offset);
void     pci_write(const struct pci_device *d, unsigned offset, uint32_t value);

/* Szuka urzadzenia vendor:device. 1 = znalezione (wynik w *out) */
int pci_find(uint16_t vendor, uint16_t device, struct pci_device *out);

#define PCI_VENDOR_ID   0x00
#define PCI_COMMAND     0x04
#define PCI_BAR0        0x10
#define PCI_INTERRUPT   0x3C          /* najmlodszy bajt: numer linii IRQ */

#define PCI_CMD_IO      0x0001        /* dostep do portow karty */
#define PCI_CMD_MEMORY  0x0002
#define PCI_CMD_MASTER  0x0004        /* karta moze sama pisac do RAM (DMA) */

#endif

#ifndef PCI_H
#define PCI_H
#include <stdint.h>

uint8_t  pci_read8 (uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg);
void     pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint16_t v);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t v);

/* first function matching class/subclass/progif; 0 and fills bus/dev/fn, -1 if none */
int pci_find(uint8_t class, uint8_t subclass, uint8_t progif,
             uint8_t *bus, uint8_t *dev, uint8_t *fn);

#endif

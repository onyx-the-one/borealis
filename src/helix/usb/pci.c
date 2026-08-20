/* pci.c — legacy CF8/CFC PCI config space access and class scan.
   Type-1 mechanism only; every target (ICH8/9 laptop, QEMU -M pc) has it.
   No resource allocation: BARs are trusted as BIOS-assigned. */
#include "pci.h"

static inline void outl(uint16_t p, uint32_t v){ __asm__ __volatile__("outl %0,%1"::"a"(v),"Nd"(p)); }
static inline uint32_t inl(uint16_t p){ uint32_t v; __asm__ __volatile__("inl %1,%0":"=a"(v):"Nd"(p)); return v; }

#define PCI_CMD 0xCF8
#define PCI_DAT 0xCFC

static uint32_t pci_addr(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg){
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
           ((uint32_t)fn << 8) | (reg & 0xFC);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg){
    outl(PCI_CMD, pci_addr(bus, dev, fn, reg));
    return inl(PCI_DAT);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg){
    return (uint16_t)(pci_read32(bus, dev, fn, reg) >> ((reg & 2) * 8));
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg){
    return (uint8_t)(pci_read32(bus, dev, fn, reg) >> ((reg & 3) * 8));
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t v){
    outl(PCI_CMD, pci_addr(bus, dev, fn, reg));
    outl(PCI_DAT, v);
}

void pci_write16(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint16_t v){
    uint32_t sh = (reg & 2) * 8;
    uint32_t cur = pci_read32(bus, dev, fn, reg);
    pci_write32(bus, dev, fn, reg, (cur & ~(0xFFFFu << sh)) | ((uint32_t)v << sh));
}

int pci_find(uint8_t class, uint8_t subclass, uint8_t progif,
             uint8_t *rbus, uint8_t *rdev, uint8_t *rfn){
    for(uint16_t bus = 0; bus < 256; bus++){
        for(uint8_t dev = 0; dev < 32; dev++){
            if(pci_read16((uint8_t)bus, dev, 0, 0x00) == 0xFFFF) continue;
            uint8_t hdr = pci_read8((uint8_t)bus, dev, 0, 0x0E);
            uint8_t last = (hdr & 0x80) ? 7 : 0;
            for(uint8_t fn = 0; fn <= last; fn++){
                if(pci_read16((uint8_t)bus, dev, fn, 0x00) == 0xFFFF) continue;
                uint32_t id = pci_read32((uint8_t)bus, dev, fn, 0x08);
                if((id >> 24) == class && ((id >> 16) & 0xFF) == subclass
                   && ((id >> 8) & 0xFF) == progif){
                    *rbus = (uint8_t)bus; *rdev = dev; *rfn = fn;
                    return 0;
                }
            }
        }
    }
    return -1;
}

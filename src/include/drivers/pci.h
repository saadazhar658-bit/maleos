#ifndef MALEOS_DRIVERS_PCI_H
#define MALEOS_DRIVERS_PCI_H

#include <stdbool.h>
#include <stdint.h>

#define PCI_MAX_DEVICES 64

#define PCI_CLASS_STORAGE 0x01
#define PCI_CLASS_BRIDGE 0x06
#define PCI_SUB_IDE 0x01
#define PCI_SUB_SATA 0x06 /* prog_if 0x01 = AHCI */

#define PCI_CMD_IO (1u << 0)
#define PCI_CMD_MEM (1u << 1)
#define PCI_CMD_BUSMASTER (1u << 2)

struct pci_bar {
    uint64_t base;
    uint64_t size; /* 0 = unused */
    bool io;       /* I/O port range, otherwise memory */
    bool prefetch;
};

struct pci_dev {
    uint8_t bus, dev, func;
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if, revision;
    uint8_t header_type;
    uint8_t irq_line;
    struct pci_bar bar[6];
    bool bound;        /* claimed by a driver */
    const char *owner; /* name of that driver */
};

/* Scan all buses reachable from the host bridge. Safe to call once at boot. */
void pci_init(void);

int pci_device_count(void);
struct pci_dev *pci_device(int index);
struct pci_dev *pci_find_class(uint8_t class_code, uint8_t subclass, int nth);

uint32_t pci_read32(const struct pci_dev *d, uint8_t off);
uint16_t pci_read16(const struct pci_dev *d, uint8_t off);
uint8_t pci_read8(const struct pci_dev *d, uint8_t off);
void pci_write32(const struct pci_dev *d, uint8_t off, uint32_t v);
void pci_write16(const struct pci_dev *d, uint8_t off, uint16_t v);

/* Turn on I/O decoding, memory decoding and/or bus mastering (PCI_CMD_*). */
void pci_enable(const struct pci_dev *d, uint16_t cmd_bits);

const char *pci_class_name(uint8_t class_code, uint8_t subclass);
void pci_dump(void);

#endif

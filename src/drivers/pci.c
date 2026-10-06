#include "drivers/pci.h"

#include "arch/cpu.h"
#include "kernel/printk.h"
#include "kernel/spinlock.h"
#include "kernel/string.h"

#define CONFIG_ADDR 0xCF8
#define CONFIG_DATA 0xCFC

static struct pci_dev devices[PCI_MAX_DEVICES];
static int dev_count;
static spinlock_t pci_lock = SPINLOCK_INIT;

static uint32_t cfg_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) | ((uint32_t)func << 8) |
           (off & 0xFC);
}

static uint32_t raw_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint64_t f = spin_lock_irqsave(&pci_lock);
    outl(CONFIG_ADDR, cfg_addr(bus, dev, func, off));
    uint32_t v = inl(CONFIG_DATA);
    spin_unlock_irqrestore(&pci_lock, f);
    return v;
}

static void raw_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off, uint32_t v)
{
    uint64_t f = spin_lock_irqsave(&pci_lock);
    outl(CONFIG_ADDR, cfg_addr(bus, dev, func, off));
    outl(CONFIG_DATA, v);
    spin_unlock_irqrestore(&pci_lock, f);
}

uint32_t pci_read32(const struct pci_dev *d, uint8_t off)
{
    return raw_read32(d->bus, d->dev, d->func, off);
}

uint16_t pci_read16(const struct pci_dev *d, uint8_t off)
{
    return (uint16_t)(pci_read32(d, off & 0xFC) >> ((off & 2) * 8));
}

uint8_t pci_read8(const struct pci_dev *d, uint8_t off)
{
    return (uint8_t)(pci_read32(d, off & 0xFC) >> ((off & 3) * 8));
}

void pci_write32(const struct pci_dev *d, uint8_t off, uint32_t v)
{
    raw_write32(d->bus, d->dev, d->func, off, v);
}

void pci_write16(const struct pci_dev *d, uint8_t off, uint16_t v)
{
    uint32_t shift = (off & 2) * 8;
    uint32_t old = pci_read32(d, off & 0xFC);
    old = (old & ~(0xFFFFu << shift)) | ((uint32_t)v << shift);
    pci_write32(d, off & 0xFC, old);
}

void pci_enable(const struct pci_dev *d, uint16_t bits)
{
    pci_write16(d, 0x04, pci_read16(d, 0x04) | bits);
}

/* Size and decode every BAR. Decoding is switched off while probing, as the spec requires. */
static void read_bars(struct pci_dev *d)
{
    int nbars = (d->header_type & 0x7F) == 0 ? 6 : ((d->header_type & 0x7F) == 1 ? 2 : 0);
    uint16_t cmd = pci_read16(d, 0x04);
    pci_write16(d, 0x04, cmd & ~(PCI_CMD_IO | PCI_CMD_MEM));

    for (int i = 0; i < nbars; i++) {
        uint8_t off = 0x10 + i * 4;
        uint32_t orig = pci_read32(d, off);
        if (orig == 0xFFFFFFFFu)
            continue;

        bool io = orig & 1;
        bool is64 = !io && ((orig >> 1) & 3) == 2;
        uint32_t orig_hi = is64 && i + 1 < nbars ? pci_read32(d, off + 4) : 0;

        pci_write32(d, off, 0xFFFFFFFFu);
        uint32_t sz = pci_read32(d, off);
        pci_write32(d, off, orig);

        uint64_t mask, base, size;
        if (io) {
            mask = sz & ~3u;
            base = orig & ~3u;
            size = (~mask + 1) & 0xFFFF;
        } else {
            mask = sz & ~0xFULL;
            base = orig & ~0xFULL;
            if (is64 && i + 1 < nbars) {
                pci_write32(d, off + 4, 0xFFFFFFFFu);
                uint32_t sz_hi = pci_read32(d, off + 4);
                pci_write32(d, off + 4, orig_hi);
                mask |= (uint64_t)sz_hi << 32;
                base |= (uint64_t)orig_hi << 32;
                size = ~mask + 1;
            } else {
                size = (~mask + 1) & 0xFFFFFFFFULL;
            }
        }

        if (mask == 0 || size == 0) {
            if (is64)
                i++;
            continue;
        }
        d->bar[i].base = base;
        d->bar[i].size = size;
        d->bar[i].io = io;
        d->bar[i].prefetch = !io && (orig & 8);
        if (is64)
            i++; /* the next BAR slot holds the upper half */
    }
    pci_write16(d, 0x04, cmd);
}

static void scan_bus(uint8_t bus);

static void scan_function(uint8_t bus, uint8_t dev, uint8_t func)
{
    uint32_t id = raw_read32(bus, dev, func, 0);
    if ((id & 0xFFFF) == 0xFFFF)
        return;
    if (dev_count == PCI_MAX_DEVICES) {
        printk("PCI: device table full, ignoring %02x:%02x.%x\n", bus, dev, func);
        return;
    }

    struct pci_dev *d = &devices[dev_count++];
    memset(d, 0, sizeof(*d));
    d->bus = bus;
    d->dev = dev;
    d->func = func;
    d->vendor = id & 0xFFFF;
    d->device = id >> 16;

    uint32_t cls = raw_read32(bus, dev, func, 0x08);
    d->revision = cls & 0xFF;
    d->prog_if = (cls >> 8) & 0xFF;
    d->subclass = (cls >> 16) & 0xFF;
    d->class_code = cls >> 24;
    d->header_type = (raw_read32(bus, dev, func, 0x0C) >> 16) & 0xFF;
    d->irq_line = raw_read32(bus, dev, func, 0x3C) & 0xFF;
    read_bars(d);

    /* PCI-to-PCI bridge: follow it to the bus behind it. */
    if (d->class_code == PCI_CLASS_BRIDGE && d->subclass == 0x04 && (d->header_type & 0x7F) == 1) {
        uint8_t secondary = (raw_read32(bus, dev, func, 0x18) >> 8) & 0xFF;
        if (secondary > bus)
            scan_bus(secondary);
    }
}

static void scan_bus(uint8_t bus)
{
    for (uint8_t dev = 0; dev < 32; dev++) {
        if ((raw_read32(bus, dev, 0, 0) & 0xFFFF) == 0xFFFF)
            continue;
        scan_function(bus, dev, 0);
        if ((raw_read32(bus, dev, 0, 0x0C) >> 16) & 0x80) { /* multi-function */
            for (uint8_t func = 1; func < 8; func++)
                scan_function(bus, dev, func);
        }
    }
}

void pci_init(void)
{
    dev_count = 0;
    scan_bus(0);
}

int pci_device_count(void)
{
    return dev_count;
}

struct pci_dev *pci_device(int index)
{
    return (index >= 0 && index < dev_count) ? &devices[index] : NULL;
}

struct pci_dev *pci_find_class(uint8_t class_code, uint8_t subclass, int nth)
{
    for (int i = 0; i < dev_count; i++) {
        if (devices[i].class_code == class_code && devices[i].subclass == subclass && nth-- == 0)
            return &devices[i];
    }
    return NULL;
}

const char *pci_class_name(uint8_t c, uint8_t s)
{
    switch (c << 8 | s) {
    case 0x0100:
        return "SCSI controller";
    case 0x0101:
        return "IDE controller";
    case 0x0106:
        return "SATA controller";
    case 0x0108:
        return "NVMe controller";
    case 0x0200:
        return "Ethernet controller";
    case 0x0300:
        return "VGA controller";
    case 0x0600:
        return "Host bridge";
    case 0x0601:
        return "ISA bridge";
    case 0x0604:
        return "PCI bridge";
    case 0x0680:
        return "Other bridge";
    case 0x0C03:
        return "USB controller";
    default:
        return "Device";
    }
}

void pci_dump(void)
{
    for (int i = 0; i < dev_count; i++) {
        const struct pci_dev *d = &devices[i];
        printk("  %02x:%02x.%x  %04x:%04x  %s%s%s\n", d->bus, d->dev, d->func, d->vendor, d->device,
               pci_class_name(d->class_code, d->subclass), d->bound ? "  -> " : "",
               d->bound ? d->owner : "");
    }
}

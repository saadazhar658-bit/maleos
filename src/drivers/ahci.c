#include "drivers/ahci.h"

#include "drivers/ata_id.h"
#include "drivers/block.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/string.h"
#include "kernel/sync.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

/*
 * AHCI (SATA) driver, DMA only, one command slot, completion by polling.
 *
 * Each port owns one frame holding the command list (1 KiB), the received-FIS area and one
 * command table, plus a 64 KiB physically contiguous bounce buffer that every transfer goes
 * through (so callers may pass any buffer and transfers are limited to 128 sectors).
 */

/* HBA registers */
#define HBA_CAP 0x00
#define HBA_GHC 0x04
#define HBA_IS 0x08
#define HBA_PI 0x0C

#define GHC_AE (1u << 31)
#define CAP_S64A (1u << 31)

/* Port registers (offset from the port base 0x100 + 0x80 * n) */
#define P_CLB 0x00
#define P_CLBU 0x04
#define P_FB 0x08
#define P_FBU 0x0C
#define P_IS 0x10
#define P_IE 0x14
#define P_CMD 0x18
#define P_TFD 0x20
#define P_SIG 0x24
#define P_SSTS 0x28
#define P_SERR 0x30
#define P_CI 0x38

#define CMD_ST (1u << 0)
#define CMD_FRE (1u << 4)
#define CMD_FR (1u << 14)
#define CMD_CR (1u << 15)

#define TFD_ERR (1u << 0)
#define TFD_DRQ (1u << 3)
#define TFD_BSY (1u << 7)
#define TFD_DF (1u << 5)

#define IS_TFES (1u << 30)

#define SATA_SIG_ATA 0x00000101

#define ATA_READ_DMA_EXT 0x25
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_READ_DMA 0xC8
#define ATA_WRITE_DMA 0xCA
#define ATA_FLUSH_EXT 0xEA
#define ATA_FLUSH 0xE7
#define ATA_IDENTIFY 0xEC

#define BOUNCE_FRAMES 16
#define BOUNCE_SECTORS ((BOUNCE_FRAMES * PAGE_SIZE) / 512)

struct cmd_header {
    uint16_t flags; /* bits 0-4: FIS length in dwords, bit 6: write */
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba, ctbau;
    uint32_t reserved[4];
} __attribute__((packed));

struct prd_entry {
    uint32_t dba, dbau, reserved, dbc;
} __attribute__((packed));

struct cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    struct prd_entry prd[1];
} __attribute__((packed));

struct ahci_port {
    volatile uint8_t *regs;
    uint64_t frame_phys;
    uint8_t *frame;
    uint64_t bounce_phys;
    uint8_t *bounce;
    bool lba48;
    struct mutex lock;
};

static int sata_index;

static inline uint32_t rd(volatile uint8_t *base, uint32_t off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void wr(volatile uint8_t *base, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

/* Poll until (reg & mask) == 0, for at most `ms` milliseconds. */
static bool wait_clear(volatile uint8_t *base, uint32_t off, uint32_t mask, uint64_t ms)
{
    uint64_t deadline = timer_ticks() + ms_to_ticks(ms) + 1;
    while (rd(base, off) & mask) {
        if (timer_ticks() >= deadline)
            return false;
        __builtin_ia32_pause();
    }
    return true;
}

static bool port_stop(volatile uint8_t *p)
{
    wr(p, P_CMD, rd(p, P_CMD) & ~(CMD_ST | CMD_FRE));
    return wait_clear(p, P_CMD, CMD_CR | CMD_FR, 500);
}

static bool port_start(volatile uint8_t *p)
{
    if (!wait_clear(p, P_TFD, TFD_BSY | TFD_DRQ, 1000))
        return false;
    wr(p, P_CMD, rd(p, P_CMD) | CMD_FRE);
    wr(p, P_CMD, rd(p, P_CMD) | CMD_ST);
    return true;
}

/* Run one command through slot 0 using the bounce buffer. Caller holds the port lock. */
static int run_command(struct ahci_port *ap, uint8_t cmd, uint64_t lba, uint32_t count, bool write,
                       uint32_t bytes, bool ext)
{
    volatile uint8_t *p = ap->regs;
    struct cmd_header *hdr = (struct cmd_header *)ap->frame;
    struct cmd_table *tbl = (struct cmd_table *)(ap->frame + 2048);

    memset(tbl, 0, sizeof(*tbl));
    hdr->flags = 5 | (write ? (1u << 6) : 0);
    hdr->prdtl = bytes ? 1 : 0;
    hdr->prdbc = 0;
    hdr->ctba = (uint32_t)(ap->frame_phys + 2048);
    hdr->ctbau = (uint32_t)((ap->frame_phys + 2048) >> 32);

    if (bytes) {
        tbl->prd[0].dba = (uint32_t)ap->bounce_phys;
        tbl->prd[0].dbau = (uint32_t)(ap->bounce_phys >> 32);
        tbl->prd[0].dbc = bytes - 1;
    }

    uint8_t *f = tbl->cfis;
    f[0] = 0x27; /* register host-to-device FIS */
    f[1] = 0x80; /* this is a command */
    f[2] = cmd;
    f[4] = lba & 0xFF;
    f[5] = (lba >> 8) & 0xFF;
    f[6] = (lba >> 16) & 0xFF;
    f[7] = 0x40 | (ext ? 0 : ((lba >> 24) & 0x0F)); /* LBA mode */
    f[8] = (lba >> 24) & 0xFF;
    f[9] = (lba >> 32) & 0xFF;
    f[10] = (lba >> 40) & 0xFF;
    f[12] = count & 0xFF;
    f[13] = (count >> 8) & 0xFF;

    if (!wait_clear(p, P_TFD, TFD_BSY | TFD_DRQ, 1000))
        return BLK_EIO;

    wr(p, P_IS, 0xFFFFFFFF);
    wr(p, P_SERR, 0xFFFFFFFF);
    __sync_synchronize();
    wr(p, P_CI, 1);

    uint64_t deadline = timer_ticks() + ms_to_ticks(5000) + 1;
    for (;;) {
        if (!(rd(p, P_CI) & 1))
            break;
        if (rd(p, P_IS) & IS_TFES)
            break;
        if (timer_ticks() >= deadline) {
            /* The device is wedged: restart the port so later commands have a chance. */
            port_stop(p);
            port_start(p);
            return BLK_EIO;
        }
        __builtin_ia32_pause();
    }

    if ((rd(p, P_IS) & IS_TFES) || (rd(p, P_TFD) & (TFD_ERR | TFD_DF))) {
        port_stop(p);
        port_start(p);
        return BLK_EIO;
    }
    return BLK_OK;
}

static int ahci_transfer(struct blockdev *dev, uint64_t lba, uint32_t count, void *rbuf,
                         const void *wbuf)
{
    struct ahci_port *ap = dev->priv;
    bool write = wbuf != NULL;
    uint8_t *rd_ptr = rbuf;
    const uint8_t *wr_ptr = wbuf;

    mutex_lock(&ap->lock);
    int rc = BLK_OK;
    while (count && rc == BLK_OK) {
        uint32_t n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
        bool ext = ap->lba48 || lba + n >= (1ULL << 28);
        if (!ap->lba48 && lba + n >= (1ULL << 28)) {
            rc = BLK_ERANGE;
            break;
        }

        if (write)
            memcpy(ap->bounce, wr_ptr, (size_t)n * 512);

        uint8_t cmd = write ? (ext ? ATA_WRITE_DMA_EXT : ATA_WRITE_DMA)
                            : (ext ? ATA_READ_DMA_EXT : ATA_READ_DMA);
        rc = run_command(ap, cmd, lba, n, write, n * 512, ext);
        if (rc == BLK_OK && write) {
            rc = run_command(ap, ap->lba48 ? ATA_FLUSH_EXT : ATA_FLUSH, 0, 0, false, 0, ap->lba48);
        }
        if (rc != BLK_OK)
            break;

        if (!write) {
            memcpy(rd_ptr, ap->bounce, (size_t)n * 512);
            rd_ptr += n * 512;
        } else {
            wr_ptr += n * 512;
        }
        lba += n;
        count -= n;
    }
    mutex_unlock(&ap->lock);
    return rc;
}

static int ahci_read(struct blockdev *dev, uint64_t lba, uint32_t count, void *buf)
{
    return ahci_transfer(dev, lba, count, buf, NULL);
}

static int ahci_write(struct blockdev *dev, uint64_t lba, uint32_t count, const void *buf)
{
    return ahci_transfer(dev, lba, count, NULL, buf);
}

static void init_port(volatile uint8_t *hba, int n, bool s64)
{
    volatile uint8_t *p = hba + 0x100 + 0x80 * n;

    uint32_t ssts = rd(p, P_SSTS);
    if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1)
        return; /* no device, or link not active */
    if (rd(p, P_SIG) != SATA_SIG_ATA)
        return; /* ATAPI, port multiplier, ... */

    if (!port_stop(p)) {
        printk("AHCI: port %d will not stop\n", n);
        return;
    }

    struct ahci_port *ap = kzalloc(sizeof(*ap));
    if (!ap)
        return;
    ap->regs = p;
    mutex_init(&ap->lock);

    ap->frame_phys = pmm_alloc_frame();
    ap->bounce_phys = pmm_alloc_frames(BOUNCE_FRAMES);
    bool too_high =
        !s64 && ((ap->frame_phys | (ap->bounce_phys + BOUNCE_FRAMES * PAGE_SIZE)) >> 32);
    if (!ap->frame_phys || !ap->bounce_phys || too_high) {
        printk("AHCI: port %d: no usable DMA memory\n", n);
        if (ap->frame_phys)
            pmm_free_frame(ap->frame_phys);
        if (ap->bounce_phys)
            pmm_free_frames(ap->bounce_phys, BOUNCE_FRAMES);
        kfree(ap);
        return;
    }
    ap->frame = phys_to_virt(ap->frame_phys);
    ap->bounce = phys_to_virt(ap->bounce_phys);
    memset(ap->frame, 0, PAGE_SIZE);

    wr(p, P_CLB, (uint32_t)ap->frame_phys);
    wr(p, P_CLBU, (uint32_t)(ap->frame_phys >> 32));
    wr(p, P_FB, (uint32_t)(ap->frame_phys + 1024));
    wr(p, P_FBU, (uint32_t)((ap->frame_phys + 1024) >> 32));
    wr(p, P_IE, 0); /* polled */
    wr(p, P_SERR, 0xFFFFFFFF);
    wr(p, P_IS, 0xFFFFFFFF);

    if (!port_start(p)) {
        printk("AHCI: port %d will not start\n", n);
        goto fail;
    }

    /* IDENTIFY DEVICE: 512 bytes of PIO-style data delivered by DMA into the bounce buffer. */
    if (run_command(ap, ATA_IDENTIFY, 0, 0, false, 512, false) != BLK_OK) {
        printk("AHCI: port %d: IDENTIFY failed\n", n);
        goto fail;
    }
    uint16_t id[256];
    memcpy(id, ap->bounce, sizeof(id));

    struct ata_ident info;
    if (ata_identify_parse(id, &info) != 0 || info.sector_size != 512)
        goto fail;
    ap->lba48 = info.lba48;

    struct blockdev *dev = kzalloc(sizeof(*dev));
    if (!dev || sata_index >= 26) {
        kfree(dev);
        goto fail;
    }
    dev->name[0] = 's';
    dev->name[1] = 'd';
    dev->name[2] = 'a' + sata_index++;
    dev->name[3] = 0;
    strlcpy(dev->model, info.model, sizeof(dev->model));
    dev->sectors = info.sectors;
    dev->sector_size = 512;
    dev->read = ahci_read;
    dev->write = ahci_write;
    dev->priv = ap;

    if (blk_register(dev) != BLK_OK) {
        kfree(dev);
        goto fail;
    }
    printk("AHCI: %s  %lu MiB  %s  (port %d)\n", dev->name, (unsigned long)(dev->sectors / 2048),
           info.model, n);
    return;

fail:
    port_stop(p);
    pmm_free_frame(ap->frame_phys);
    pmm_free_frames(ap->bounce_phys, BOUNCE_FRAMES);
    kfree(ap);
}

static int ahci_probe(struct pci_dev *pci)
{
    const struct pci_bar *abar = &pci->bar[5];
    if (!abar->size || abar->io)
        return -1;

    pci_enable(pci, PCI_CMD_MEM | PCI_CMD_BUSMASTER);
    volatile uint8_t *hba = vmm_ioremap(abar->base, abar->size < 0x1100 ? 0x1100 : abar->size);
    if (!hba)
        return -1;

    wr(hba, HBA_GHC, rd(hba, HBA_GHC) | GHC_AE);
    uint32_t cap = rd(hba, HBA_CAP);
    uint32_t pi = rd(hba, HBA_PI);
    bool s64 = cap & CAP_S64A;

    for (int n = 0; n < 32; n++) {
        if (pi & (1u << n))
            init_port(hba, n, s64);
    }
    return 0;
}

static const struct pci_match ahci_ids[] = {
    {.flags = MATCH_CLASS | MATCH_SUBCLASS | MATCH_PROG_IF,
     .class_code = PCI_CLASS_STORAGE,
     .subclass = PCI_SUB_SATA,
     .prog_if = 0x01},
    {0},
};

const struct driver ahci_driver = {
    .name = "ahci",
    .match = ahci_ids,
    .probe_pci = ahci_probe,
};

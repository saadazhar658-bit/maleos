#include "drivers/ata.h"

#include "arch/cpu.h"
#include "drivers/ata_id.h"
#include "drivers/block.h"
#include "kernel/printk.h"
#include "kernel/string.h"
#include "kernel/sync.h"
#include "mm/heap.h"

/*
 * IDE/PATA driver using programmed I/O. Interrupts from the drive are disabled (nIEN) and
 * completion is detected by polling the status register, which keeps the driver simple and
 * is fast enough for the sector counts used here. Each channel is serialized by a mutex
 * because its two drives share one set of registers.
 */

#define ATA_SR_ERR 0x01
#define ATA_SR_DRQ 0x08
#define ATA_SR_DF 0x20
#define ATA_SR_DRDY 0x40
#define ATA_SR_BSY 0x80

#define ATA_CMD_READ28 0x20
#define ATA_CMD_WRITE28 0x30
#define ATA_CMD_READ48 0x24
#define ATA_CMD_WRITE48 0x34
#define ATA_CMD_FLUSH28 0xE7
#define ATA_CMD_FLUSH48 0xEA
#define ATA_CMD_IDENTIFY 0xEC

#define ATA_MAX_CHUNK 128 /* sectors per command */
#define ATA_SPIN 4000000

int ata_force_lba28;

struct ata_chan {
    uint16_t cmd, ctrl;
    struct mutex lock;
};

struct ata_drive {
    struct ata_chan *chan;
    int slave;
    bool lba48;
};

static int drive_index; /* hda, hdb, ... across all controllers */

static inline uint8_t status(struct ata_chan *c)
{
    return inb(c->cmd + 7);
}

static void delay_400ns(struct ata_chan *c)
{
    for (int i = 0; i < 4; i++)
        (void)inb(c->ctrl); /* alternate status: reading it costs ~100 ns each */
}

/* Wait until BSY clears. Returns the final status, or -1 on timeout. */
static int wait_not_busy(struct ata_chan *c)
{
    for (int i = 0; i < ATA_SPIN; i++) {
        uint8_t s = status(c);
        if (!(s & ATA_SR_BSY))
            return s;
        __builtin_ia32_pause();
    }
    return -1;
}

/* Wait for DRQ (data ready). Returns 0, or BLK_EIO on error/timeout. */
static int wait_drq(struct ata_chan *c)
{
    for (int i = 0; i < ATA_SPIN; i++) {
        uint8_t s = status(c);
        if (s & (ATA_SR_ERR | ATA_SR_DF))
            return BLK_EIO;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ))
            return BLK_OK;
        __builtin_ia32_pause();
    }
    return BLK_EIO;
}

static void select_drive(struct ata_chan *c, int slave, uint8_t extra)
{
    outb(c->cmd + 6, 0xA0 | extra | (slave << 4));
    delay_400ns(c);
}

/* Program the task file for a transfer and issue the command. */
static int issue(struct ata_drive *d, uint64_t lba, uint32_t count, bool write, bool use48)
{
    struct ata_chan *c = d->chan;

    if (wait_not_busy(c) < 0)
        return BLK_EIO;

    if (use48) {
        select_drive(c, d->slave, 0x40);
        outb(c->cmd + 2, (count >> 8) & 0xFF);
        outb(c->cmd + 3, (lba >> 24) & 0xFF);
        outb(c->cmd + 4, (lba >> 32) & 0xFF);
        outb(c->cmd + 5, (lba >> 40) & 0xFF);
        outb(c->cmd + 2, count & 0xFF);
        outb(c->cmd + 3, lba & 0xFF);
        outb(c->cmd + 4, (lba >> 8) & 0xFF);
        outb(c->cmd + 5, (lba >> 16) & 0xFF);
        outb(c->cmd + 7, write ? ATA_CMD_WRITE48 : ATA_CMD_READ48);
    } else {
        select_drive(c, d->slave, 0x40 | ((lba >> 24) & 0x0F));
        outb(c->cmd + 2, count & 0xFF); /* 0 would mean 256; chunks never exceed 128 */
        outb(c->cmd + 3, lba & 0xFF);
        outb(c->cmd + 4, (lba >> 8) & 0xFF);
        outb(c->cmd + 5, (lba >> 16) & 0xFF);
        outb(c->cmd + 7, write ? ATA_CMD_WRITE28 : ATA_CMD_READ28);
    }
    return BLK_OK;
}

static bool use_lba48(struct ata_drive *d, uint64_t lba, uint32_t count)
{
    if (lba + count > (1ULL << 28) - 1)
        return true; /* only reachable with the 48-bit commands */
    return d->lba48 && !ata_force_lba28;
}

static int ata_read(struct blockdev *dev, uint64_t lba, uint32_t count, void *buf)
{
    struct ata_drive *d = dev->priv;
    uint8_t *out = buf;
    mutex_lock(&d->chan->lock);

    int rc = BLK_OK;
    while (count && rc == BLK_OK) {
        uint32_t n = count > ATA_MAX_CHUNK ? ATA_MAX_CHUNK : count;
        rc = issue(d, lba, n, false, use_lba48(d, lba, n));
        for (uint32_t i = 0; i < n && rc == BLK_OK; i++) {
            rc = wait_drq(d->chan);
            if (rc == BLK_OK) {
                insw(d->chan->cmd, out, 256);
                out += 512;
            }
        }
        lba += n;
        count -= n;
    }
    mutex_unlock(&d->chan->lock);
    return rc;
}

static int ata_write(struct blockdev *dev, uint64_t lba, uint32_t count, const void *buf)
{
    struct ata_drive *d = dev->priv;
    const uint8_t *in = buf;
    mutex_lock(&d->chan->lock);

    int rc = BLK_OK;
    while (count && rc == BLK_OK) {
        uint32_t n = count > ATA_MAX_CHUNK ? ATA_MAX_CHUNK : count;
        bool use48 = use_lba48(d, lba, n);
        rc = issue(d, lba, n, true, use48);
        for (uint32_t i = 0; i < n && rc == BLK_OK; i++) {
            rc = wait_drq(d->chan);
            if (rc == BLK_OK) {
                outsw(d->chan->cmd, in, 256);
                in += 512;
            }
        }
        if (rc == BLK_OK) { /* make sure the data reaches the platters */
            outb(d->chan->cmd + 7, use48 ? ATA_CMD_FLUSH48 : ATA_CMD_FLUSH28);
            int s = wait_not_busy(d->chan);
            if (s < 0 || (s & (ATA_SR_ERR | ATA_SR_DF)))
                rc = BLK_EIO;
        }
        lba += n;
        count -= n;
    }
    mutex_unlock(&d->chan->lock);
    return rc;
}

/* Probe one drive position; registers a block device if a usable disk answers. */
static void probe_drive(struct ata_chan *c, int slave)
{
    select_drive(c, slave, 0);
    uint8_t s = status(c);
    if (s == 0xFF || s == 0x00)
        return; /* floating bus: nothing attached */

    outb(c->cmd + 2, 0);
    outb(c->cmd + 3, 0);
    outb(c->cmd + 4, 0);
    outb(c->cmd + 5, 0);
    outb(c->cmd + 7, ATA_CMD_IDENTIFY);
    if (status(c) == 0)
        return;

    int st = wait_not_busy(c);
    if (st < 0)
        return;
    if (inb(c->cmd + 4) || inb(c->cmd + 5))
        return; /* ATAPI or SATA signature: not a plain ATA disk */
    if (wait_drq(c) != BLK_OK)
        return;

    uint16_t id[256];
    insw(c->cmd, id, 256);

    struct ata_ident info;
    if (ata_identify_parse(id, &info) != 0)
        return;
    if (info.sector_size != 512) {
        printk("ATA: skipping a drive with %u-byte sectors\n", info.sector_size);
        return;
    }

    struct blockdev *dev = kzalloc(sizeof(*dev));
    struct ata_drive *d = kzalloc(sizeof(*d));
    if (!dev || !d || drive_index >= 26) {
        kfree(dev);
        kfree(d);
        return;
    }
    d->chan = c;
    d->slave = slave;
    d->lba48 = info.lba48;

    dev->name[0] = 'h';
    dev->name[1] = 'd';
    dev->name[2] = 'a' + drive_index++;
    dev->name[3] = 0;
    strlcpy(dev->model, info.model, sizeof(dev->model));
    dev->sectors = info.sectors;
    dev->sector_size = 512;
    dev->read = ata_read;
    dev->write = ata_write;
    dev->priv = d;

    if (blk_register(dev) != BLK_OK) {
        kfree(dev);
        kfree(d);
        return;
    }
    printk("ATA: %s  %lu MiB  %s%s\n", dev->name, (unsigned long)(dev->sectors / 2048), info.model,
           info.lba48 ? "  (LBA48)" : "");
}

static int ata_probe(struct pci_dev *pci)
{
    pci_enable(pci, PCI_CMD_IO | PCI_CMD_BUSMASTER);

    for (int ch = 0; ch < 2; ch++) {
        /* prog_if bit 0 / bit 2 clear = that channel runs in legacy "compatibility" mode. */
        bool native = pci->prog_if & (ch == 0 ? 0x01 : 0x04);
        uint16_t cmd, ctrl;
        if (native) {
            const struct pci_bar *bc = &pci->bar[ch * 2], *bt = &pci->bar[ch * 2 + 1];
            if (!bc->io || !bt->io)
                continue;
            cmd = (uint16_t)bc->base;
            ctrl = (uint16_t)bt->base + 2;
        } else {
            cmd = ch == 0 ? 0x1F0 : 0x170;
            ctrl = ch == 0 ? 0x3F6 : 0x376;
        }

        struct ata_chan *c = kzalloc(sizeof(*c));
        if (!c)
            return -1;
        c->cmd = cmd;
        c->ctrl = ctrl;
        mutex_init(&c->lock);
        outb(c->ctrl, 0x02); /* nIEN: we poll, no interrupts */

        int before = blk_count();
        probe_drive(c, 0);
        probe_drive(c, 1);
        if (blk_count() == before)
            kfree(c); /* no drive on this channel */
    }
    return 0;
}

static const struct pci_match ata_ids[] = {
    {.flags = MATCH_CLASS | MATCH_SUBCLASS,
     .class_code = PCI_CLASS_STORAGE,
     .subclass = PCI_SUB_IDE},
    {0},
};

const struct driver ata_driver = {
    .name = "ata-pio",
    .match = ata_ids,
    .probe_pci = ata_probe,
};

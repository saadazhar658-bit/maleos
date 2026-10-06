#include "drivers/block.h"

#include <stddef.h>

#include "kernel/printk.h"
#include "kernel/spinlock.h"
#include "kernel/string.h"
#include "mm/heap.h"

static struct blockdev *devices[BLK_MAX_DEVICES];
static int count;
static spinlock_t blk_lock = SPINLOCK_INIT;

int blk_register(struct blockdev *dev)
{
    if (!dev || !dev->read || dev->sectors == 0 || dev->sector_size == 0 ||
        (dev->sector_size & (dev->sector_size - 1)))
        return BLK_EINVAL;
    if (!dev->write)
        dev->readonly = 1;

    uint64_t f = spin_lock_irqsave(&blk_lock);
    int rc = BLK_OK;
    if (count == BLK_MAX_DEVICES) {
        rc = BLK_EINVAL;
    } else {
        for (int i = 0; i < count; i++) {
            if (strcmp(devices[i]->name, dev->name) == 0)
                rc = BLK_EINVAL; /* names must be unique */
        }
        if (rc == BLK_OK)
            devices[count++] = dev;
    }
    spin_unlock_irqrestore(&blk_lock, f);
    return rc;
}

void blk_unregister(struct blockdev *dev)
{
    uint64_t f = spin_lock_irqsave(&blk_lock);
    for (int i = 0; i < count; i++) {
        if (devices[i] == dev) {
            for (int j = i; j + 1 < count; j++)
                devices[j] = devices[j + 1];
            count--;
            break;
        }
    }
    spin_unlock_irqrestore(&blk_lock, f);
}

int blk_count(void)
{
    return count;
}

struct blockdev *blk_at(int index)
{
    return (index >= 0 && index < count) ? devices[index] : NULL;
}

struct blockdev *blk_find(const char *name)
{
    for (int i = 0; i < count; i++) {
        if (strcmp(devices[i]->name, name) == 0)
            return devices[i];
    }
    return NULL;
}

static int check_range(const struct blockdev *dev, uint64_t lba, uint32_t n)
{
    if (lba > dev->sectors || n > dev->sectors - lba)
        return BLK_ERANGE;
    return BLK_OK;
}

int blk_read(struct blockdev *dev, uint64_t lba, uint32_t n, void *buf)
{
    if (!dev || (n && !buf))
        return BLK_EINVAL;
    int rc = check_range(dev, lba, n);
    if (rc != BLK_OK || n == 0)
        return rc;
    rc = dev->read(dev, lba, n, buf);
    if (rc == BLK_OK)
        dev->reads++;
    return rc;
}

int blk_write(struct blockdev *dev, uint64_t lba, uint32_t n, const void *buf)
{
    if (!dev || (n && !buf))
        return BLK_EINVAL;
    if (dev->readonly || !dev->write)
        return BLK_ERO;
    int rc = check_range(dev, lba, n);
    if (rc != BLK_OK || n == 0)
        return rc;
    rc = dev->write(dev, lba, n, buf);
    if (rc == BLK_OK)
        dev->writes++;
    return rc;
}

int blk_read_bytes(struct blockdev *dev, uint64_t offset, size_t len, void *buf)
{
    if (!dev || (len && !buf))
        return BLK_EINVAL;
    if (len == 0)
        return BLK_OK;

    uint64_t ss = dev->sector_size;
    uint64_t total = dev->sectors * ss;
    if (offset > total || len > total - offset)
        return BLK_ERANGE;

    uint8_t *out = buf;
    uint8_t *bounce = NULL;

    while (len) {
        uint64_t lba = offset / ss;
        uint64_t within = offset % ss;

        if (within == 0 && len >= ss) { /* aligned run: read straight into the caller's buffer */
            uint64_t nsec = len / ss;
            if (nsec > 128)
                nsec = 128;
            int rc = blk_read(dev, lba, (uint32_t)nsec, out);
            if (rc != BLK_OK) {
                kfree(bounce);
                return rc;
            }
            out += nsec * ss;
            offset += nsec * ss;
            len -= nsec * ss;
            continue;
        }

        if (!bounce) {
            bounce = kmalloc(ss);
            if (!bounce)
                return BLK_EIO;
        }
        int rc = blk_read(dev, lba, 1, bounce);
        if (rc != BLK_OK) {
            kfree(bounce);
            return rc;
        }
        size_t take = ss - within;
        if (take > len)
            take = len;
        memcpy(out, bounce + within, take);
        out += take;
        offset += take;
        len -= take;
    }
    kfree(bounce);
    return BLK_OK;
}

const char *blk_strerror(int err)
{
    switch (err) {
    case BLK_OK:
        return "ok";
    case BLK_EIO:
        return "I/O error";
    case BLK_ERANGE:
        return "out of range";
    case BLK_EINVAL:
        return "invalid argument";
    case BLK_ERO:
        return "read-only device";
    default:
        return "unknown error";
    }
}

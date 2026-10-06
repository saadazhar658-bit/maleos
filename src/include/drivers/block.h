#ifndef MALEOS_DRIVERS_BLOCK_H
#define MALEOS_DRIVERS_BLOCK_H

#include <stddef.h>
#include <stdint.h>

#define BLK_OK 0
#define BLK_EIO (-1)    /* the device reported an error or timed out */
#define BLK_ERANGE (-2) /* access past the end of the device */
#define BLK_EINVAL (-3)
#define BLK_ERO (-4) /* write to a read-only device */

#define BLK_NAME_MAX 8
#define BLK_MODEL_MAX 41
#define BLK_MAX_DEVICES 8

/*
 * A block device: an array of fixed-size sectors addressed by LBA. Drivers fill in this
 * structure and register it; everything above (filesystems, tests) only uses the blk_*
 * functions, which validate arguments before calling the driver.
 */
struct blockdev {
    char name[BLK_NAME_MAX]; /* "hda", "sda", ... */
    char model[BLK_MODEL_MAX];
    uint64_t sectors;
    uint32_t sector_size;
    int readonly;

    int (*read)(struct blockdev *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blockdev *dev, uint64_t lba, uint32_t count, const void *buf);
    void *priv;

    uint64_t reads, writes; /* completed requests (statistics) */
};

int blk_register(struct blockdev *dev);
void blk_unregister(struct blockdev *dev);
int blk_count(void);
struct blockdev *blk_at(int index);
struct blockdev *blk_find(const char *name);

/* Sector-granular access. `count` may be 0 (no-op). */
int blk_read(struct blockdev *dev, uint64_t lba, uint32_t count, void *buf);
int blk_write(struct blockdev *dev, uint64_t lba, uint32_t count, const void *buf);

/* Byte-granular read at any offset and length (used by filesystems). */
int blk_read_bytes(struct blockdev *dev, uint64_t offset, size_t len, void *buf);

const char *blk_strerror(int err);

#endif

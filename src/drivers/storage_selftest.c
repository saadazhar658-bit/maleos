#include "arch/cpu.h"
#include "drivers/ata.h"
#include "drivers/block.h"
#include "drivers/selftest.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/string.h"
#include "mm/heap.h"

static int checks;

#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond))                                                                               \
            kpanic("storage selftest failed: %s (%s:%d)", what, __FILE__, __LINE__);               \
    } while (0)

#define SS 512

static uint8_t pat(uint64_t sector, uint32_t j)
{
    return (uint8_t)(sector * 131 + j * 7 + 13);
}

static void fill_pattern(uint8_t *buf, uint64_t lba, uint32_t count)
{
    for (uint32_t s = 0; s < count; s++) {
        for (uint32_t j = 0; j < SS; j++)
            buf[s * SS + j] = pat(lba + s, j);
    }
}

/* ---------- block layer, using a RAM-backed device (no hardware needed) ---------- */

struct ramdisk {
    uint8_t *data;
    uint64_t sectors;
};

static int ram_read(struct blockdev *d, uint64_t lba, uint32_t n, void *buf)
{
    struct ramdisk *r = d->priv;
    memcpy(buf, r->data + lba * SS, (size_t)n * SS);
    return BLK_OK;
}

static int ram_write(struct blockdev *d, uint64_t lba, uint32_t n, const void *buf)
{
    struct ramdisk *r = d->priv;
    memcpy(r->data + lba * SS, buf, (size_t)n * SS);
    return BLK_OK;
}

static void test_block_layer(void)
{
    struct ramdisk r = {.sectors = 64};
    r.data = kzalloc(r.sectors * SS);
    CHECK(r.data != NULL, "ram disk memory");

    struct blockdev rw = {.name = "ramrw",
                          .sectors = r.sectors,
                          .sector_size = SS,
                          .read = ram_read,
                          .write = ram_write,
                          .priv = &r};
    struct blockdev ro = {
        .name = "ramro", .sectors = r.sectors, .sector_size = SS, .read = ram_read, .priv = &r};
    struct blockdev dup = rw;
    struct blockdev bad = {.name = "bad", .sectors = 0, .sector_size = SS, .read = ram_read};
    struct blockdev odd = {.name = "odd", .sectors = 8, .sector_size = 500, .read = ram_read};

    int before = blk_count();
    CHECK(blk_register(&rw) == BLK_OK, "register");
    CHECK(blk_register(&ro) == BLK_OK, "register read-only");
    CHECK(blk_register(&dup) == BLK_EINVAL, "duplicate name rejected");
    CHECK(blk_register(&bad) == BLK_EINVAL, "zero-size device rejected");
    CHECK(blk_register(&odd) == BLK_EINVAL, "non-power-of-two sector size rejected");
    CHECK(blk_count() == before + 2, "count after registering");
    CHECK(blk_find("ramrw") == &rw && blk_find("nonexistent") == NULL, "find by name");
    CHECK(ro.readonly == 1, "device without write() is read-only");

    uint8_t buf[4 * SS], back[4 * SS];
    fill_pattern(buf, 10, 4);
    CHECK(blk_write(&rw, 10, 4, buf) == BLK_OK, "write");
    CHECK(blk_read(&rw, 10, 4, back) == BLK_OK && memcmp(buf, back, sizeof(buf)) == 0,
          "read back what was written");
    CHECK(rw.writes == 1 && rw.reads == 1, "statistics");

    CHECK(blk_write(&ro, 0, 1, buf) == BLK_ERO, "write to read-only device refused");
    CHECK(blk_read(&rw, 0, 0, NULL) == BLK_OK, "zero-length read with NULL buffer");
    CHECK(blk_read(&rw, 0, 1, NULL) == BLK_EINVAL, "NULL buffer rejected");
    CHECK(blk_read(NULL, 0, 1, buf) == BLK_EINVAL, "NULL device rejected");
    CHECK(blk_read(&rw, 64, 0, buf) == BLK_OK, "empty read at the very end");
    CHECK(blk_read(&rw, 63, 1, buf) == BLK_OK, "last sector readable");
    CHECK(blk_read(&rw, 63, 2, buf) == BLK_ERANGE, "read running past the end");
    CHECK(blk_read(&rw, 65, 0, buf) == BLK_ERANGE, "start beyond the end");
    CHECK(blk_read(&rw, ~0ULL, 2, buf) == BLK_ERANGE, "overflowing lba rejected");
    CHECK(blk_write(&rw, 62, 4, buf) == BLK_ERANGE, "write past the end");

    /* Byte-granular reads: unaligned start, unaligned end, spanning several sectors. */
    uint8_t all[8 * SS], part[1500];
    fill_pattern(all, 10, 8);
    CHECK(blk_write(&rw, 10, 8, all) == BLK_OK, "write 8 sectors");
    CHECK(blk_read_bytes(&rw, 10 * SS + 100, 1500, part) == BLK_OK &&
              memcmp(part, all + 100, 1500) == 0,
          "unaligned byte read across sectors");
    CHECK(blk_read_bytes(&rw, 10 * SS + 510, 4, part) == BLK_OK && memcmp(part, all + 510, 4) == 0,
          "byte read straddling a sector boundary");
    CHECK(blk_read_bytes(&rw, 10 * SS, 3 * SS, back) == BLK_OK, "aligned byte read");
    CHECK(blk_read_bytes(&rw, 64 * SS - 2, 4, part) == BLK_ERANGE, "byte read past the end");
    CHECK(blk_read_bytes(&rw, 0, 0, NULL) == BLK_OK, "empty byte read");

    blk_unregister(&rw);
    blk_unregister(&ro);
    CHECK(blk_count() == before && blk_find("ramrw") == NULL, "unregister");
    kfree(r.data);
}

/* ---------- ATA IDENTIFY parsing with synthetic data ---------- */

#include "drivers/ata_id.h"

static void set_string(uint16_t *w, const char *s, int words)
{
    for (int i = 0; i < words; i++) {
        char a = *s ? *s++ : ' ';
        char b = *s ? *s++ : ' ';
        w[i] = ((uint16_t)(uint8_t)a << 8) | (uint8_t)b;
    }
}

static void test_identify_parser(void)
{
    uint16_t id[256];
    struct ata_ident info;

    memset(id, 0, sizeof(id));
    set_string(&id[27], "TEST DISK 9000", 20);
    id[49] = 1 << 9;
    id[60] = 0x1000;
    id[61] = 0x0002; /* 0x21000 sectors with LBA28 */
    CHECK(ata_identify_parse(id, &info) == 0, "LBA28 drive parses");
    CHECK(info.sectors == 0x21000 && !info.lba48 && info.sector_size == 512, "LBA28 size");
    CHECK(strcmp(info.model, "TEST DISK 9000") == 0, "model string unswapped and trimmed");

    id[83] = 1 << 10;
    id[100] = 0x0000;
    id[101] = 0x0000;
    id[102] = 0x0001; /* 1 << 32 sectors = 2 TiB */
    CHECK(ata_identify_parse(id, &info) == 0 && info.lba48, "LBA48 drive parses");
    CHECK(info.sectors == (1ULL << 32), "LBA48 size wins over the 28-bit field");

    id[106] = 0x4000 | (1 << 12);
    id[117] = 2048;
    CHECK(ata_identify_parse(id, &info) == 0 && info.sector_size == 4096, "4 KiB logical sectors");

    id[0] = 0x8000;
    CHECK(ata_identify_parse(id, &info) < 0, "ATAPI device rejected");
    id[0] = 0;
    id[49] = 0;
    CHECK(ata_identify_parse(id, &info) < 0, "CHS-only drive rejected");
    id[49] = 1 << 9;
    id[60] = id[61] = id[83] = 0;
    CHECK(ata_identify_parse(id, &info) < 0, "zero-sized drive rejected");
}

/* ---------- real disks ---------- */

static const uint64_t SCRATCH = 14000; /* inside the raw area after the 6 MiB filesystem */

static void test_disk(struct blockdev *d, const char *expect_label)
{
    printk("  testing %s (%s)\n", d->name, d->model);
    CHECK(d->sectors == 16384 && d->sector_size == 512, "geometry of the 8 MiB test image");
    CHECK(strncmp(d->model, "QEMU", 4) == 0, "model string");

    /* Superblock: ext2 magic 0xEF53 at byte 1024 + 56. */
    uint8_t magic[2];
    CHECK(blk_read_bytes(d, 1024 + 56, 2, magic) == BLK_OK, "read the superblock");
    CHECK(magic[0] == 0x53 && magic[1] == 0xEF, "ext2 magic number");

    /* Volume label proves we are looking at the right image. */
    char label[17] = {0};
    CHECK(blk_read_bytes(d, 1024 + 120, 16, label) == BLK_OK, "read the volume label");
    CHECK(strcmp(label, expect_label) == 0, "volume label matches the image");

    /* Range checks against the real size. */
    uint8_t one[SS];
    CHECK(blk_read(d, d->sectors - 1, 1, one) == BLK_OK, "last sector");
    CHECK(blk_read(d, d->sectors, 1, one) == BLK_ERANGE, "past the end");

    /* A multi-chunk read (300 sectors > 128 per command) equals three separate reads. */
    uint8_t *big = kmalloc(300 * SS), *parts = kmalloc(300 * SS);
    CHECK(big && parts, "buffers");
    CHECK(blk_read(d, 2, 300, big) == BLK_OK, "300-sector read");
    int same = 1;
    for (int i = 0; i < 3; i++)
        same &= (blk_read(d, 2 + i * 100, 100, parts + i * 100 * SS) == BLK_OK);
    CHECK(same && memcmp(big, parts, 300 * SS) == 0, "chunked read equals piecewise reads");

    /* Write / read-back across several command chunks, then a different pattern on top. */
    uint8_t *w = kmalloc(300 * SS), *r = kmalloc(300 * SS);
    CHECK(w && r, "write buffers");
    fill_pattern(w, SCRATCH, 300);
    CHECK(blk_write(d, SCRATCH, 300, w) == BLK_OK, "write 300 sectors");
    memset(r, 0, 300 * SS);
    CHECK(blk_read(d, SCRATCH, 300, r) == BLK_OK && memcmp(w, r, 300 * SS) == 0,
          "read back 300 written sectors");
    fill_pattern(w, SCRATCH + 1000, 1);
    CHECK(blk_write(d, SCRATCH + 50, 1, w) == BLK_OK, "overwrite a single sector");
    CHECK(blk_read(d, SCRATCH + 49, 3, r) == BLK_OK, "read around it");
    uint8_t expect[3 * SS];
    fill_pattern(expect, SCRATCH + 49, 3);
    memcpy(expect + SS, w, SS);
    CHECK(memcmp(r, expect, sizeof(expect)) == 0, "neighbours untouched by a single-sector write");

    /* The filesystem area must be unharmed by all of the above. */
    CHECK(blk_read_bytes(d, 1024 + 56, 2, magic) == BLK_OK && magic[0] == 0x53 && magic[1] == 0xEF,
          "superblock intact");

    kfree(big);
    kfree(parts);
    kfree(w);
    kfree(r);
}

/* Two threads hammer one disk at the same time; the per-device lock must keep them apart. */
static struct blockdev *conc_dev;
static uint8_t *conc_ref[2];
static volatile int conc_failures;
static const uint64_t conc_lba[2] = {0, 3000};

static void conc_reader(void *arg)
{
    int k = (int)(uintptr_t)arg;
    uint8_t *buf = kmalloc(64 * SS);
    for (int i = 0; i < 25; i++) {
        if (blk_read(conc_dev, conc_lba[k], 64, buf) != BLK_OK ||
            memcmp(buf, conc_ref[k], 64 * SS) != 0)
            conc_failures++;
        if (i % 5 == 0)
            sched_yield();
    }
    kfree(buf);
}

static void test_concurrency(struct blockdev *d)
{
    conc_dev = d;
    conc_failures = 0;
    for (int k = 0; k < 2; k++) {
        conc_ref[k] = kmalloc(64 * SS);
        CHECK(blk_read(d, conc_lba[k], 64, conc_ref[k]) == BLK_OK, "reference read");
    }
    struct thread *t[3];
    for (int k = 0; k < 2; k++)
        t[k] = thread_create("blk-reader", conc_reader, (void *)(uintptr_t)k, SCHED_PRIO_NORMAL);
    t[2] = thread_create("blk-reader", conc_reader, (void *)(uintptr_t)0, SCHED_PRIO_NORMAL);
    for (int k = 0; k < 3; k++)
        thread_join(t[k]);
    CHECK(conc_failures == 0, "concurrent readers always get their own data");
    kfree(conc_ref[0]);
    kfree(conc_ref[1]);
}

static void test_ata_lba28(struct blockdev *d)
{
    uint8_t *a = kmalloc(200 * SS), *b = kmalloc(200 * SS), *w = kmalloc(200 * SS);
    CHECK(a && b && w, "buffers");

    CHECK(blk_read(d, 100, 200, a) == BLK_OK, "LBA48 read");
    ata_force_lba28 = 1;
    int rc = blk_read(d, 100, 200, b);
    CHECK(rc == BLK_OK && memcmp(a, b, 200 * SS) == 0, "28-bit commands read identical data");

    fill_pattern(w, SCRATCH + 2000, 200);
    CHECK(blk_write(d, SCRATCH + 500, 200, w) == BLK_OK, "28-bit write");
    ata_force_lba28 = 0;
    CHECK(blk_read(d, SCRATCH + 500, 200, a) == BLK_OK && memcmp(a, w, 200 * SS) == 0,
          "48-bit read sees the 28-bit write");
    kfree(a);
    kfree(b);
    kfree(w);
}

int storage_selftest(void)
{
    checks = 0;
    test_block_layer();
    test_identify_parser();

    struct blockdev *hda = blk_find("hda");
    struct blockdev *sda = blk_find("sda");
    if (!hda && !sda) {
        printk("STORAGE SELFTEST: no disks attached, hardware tests skipped\n");
        return -1;
    }

    if (hda) {
        test_disk(hda, "ide");
        test_ata_lba28(hda);
        test_concurrency(hda);
    }
    if (sda) {
        test_disk(sda, "sata");
        test_concurrency(sda);
    }
    return checks;
}

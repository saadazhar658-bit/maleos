#include "fs/ext2.h"

#include "kernel/printk.h"
#include "kernel/string.h"
#include "mm/heap.h"

/*
 * Read-only ext2. All on-disk fields are little endian, as is x86, so they are read with
 * plain loads from byte buffers. Every offset taken from the disk is validated before use:
 * a corrupt image must produce -EIO, never a wild access.
 */

#define EXT2_MAGIC 0xEF53
#define EXT2_ROOT_INO 2
#define INCOMPAT_FILETYPE 0x0002
#define INCOMPAT_FLEX_BG 0x0200
#define INCOMPAT_SUPPORTED (INCOMPAT_FILETYPE | INCOMPAT_FLEX_BG)
#define BCACHE_ENTRIES 16
#define DIR_ENTRY_MIN 8

struct bcache_entry {
    uint32_t blk;
    bool valid;
    uint8_t *data;
};

struct ext2_sb {
    struct blockdev *dev;
    uint32_t block_size;
    uint32_t blocks_count;
    uint32_t first_data_block;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t inodes_count;
    uint32_t inode_size;
    uint32_t group_count;
    struct bcache_entry cache[BCACHE_ENTRIES];
    unsigned cache_next;
    struct enode *nodes; /* live vnodes, so one inode is always one vnode */
};

struct enode {
    struct vnode vn;
    struct enode *next;
    uint32_t block[15];
    uint32_t i_blocks;
    uint32_t file_acl;
};

static const struct vnode_ops ext2_ops;

static inline uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static struct ext2_sb *esb(const struct vnode *v)
{
    return v->sb->priv;
}

/* ---------- block access through a small cache ---------- */

static const uint8_t *get_block(struct ext2_sb *s, uint32_t blk)
{
    if (blk >= s->blocks_count)
        return NULL;
    for (int i = 0; i < BCACHE_ENTRIES; i++) {
        if (s->cache[i].valid && s->cache[i].blk == blk)
            return s->cache[i].data;
    }
    struct bcache_entry *e = &s->cache[s->cache_next];
    s->cache_next = (s->cache_next + 1) % BCACHE_ENTRIES;
    e->valid = false;
    if (blk_read_bytes(s->dev, (uint64_t)blk * s->block_size, s->block_size, e->data) < 0)
        return NULL;
    e->blk = blk;
    e->valid = true;
    return e->data;
}

/* Map a file's logical block to a disk block. 0 means a hole; *err is set on corruption. */
static uint32_t bmap(struct enode *n, uint64_t lblk, int *err)
{
    struct ext2_sb *s = esb(&n->vn);
    uint64_t per = s->block_size / 4;
    *err = 0;

    if (lblk < 12)
        return n->block[lblk];
    lblk -= 12;

    int level;
    uint32_t root;
    if (lblk < per) {
        level = 1;
        root = n->block[12];
    } else if ((lblk -= per) < per * per) {
        level = 2;
        root = n->block[13];
    } else if ((lblk -= per * per) < per * per * per) {
        level = 3;
        root = n->block[14];
    } else {
        *err = -EFBIG;
        return 0;
    }

    uint32_t blk = root;
    for (int l = level; l > 0 && blk; l--) {
        uint64_t div = 1;
        for (int i = 1; i < l; i++)
            div *= per;
        const uint8_t *tbl = get_block(s, blk);
        if (!tbl) {
            *err = -EIO;
            return 0;
        }
        blk = le32(tbl + 4 * ((lblk / div) % per));
    }
    return blk;
}

/* Read bytes of a file or directory; holes read as zeros. */
static int64_t file_read(struct enode *n, uint64_t off, void *buf, size_t len)
{
    struct ext2_sb *s = esb(&n->vn);
    uint8_t *out = buf;
    size_t done = 0;

    while (done < len) {
        uint64_t pos = off + done;
        size_t in_blk = pos % s->block_size;
        size_t chunk = s->block_size - in_blk;
        if (chunk > len - done)
            chunk = len - done;

        int err;
        uint32_t phys = bmap(n, pos / s->block_size, &err);
        if (err)
            return err;
        if (phys == 0) {
            memset(out + done, 0, chunk);
        } else {
            const uint8_t *b = get_block(s, phys);
            if (!b)
                return -EIO;
            memcpy(out + done, b + in_blk, chunk);
        }
        done += chunk;
    }
    return (int64_t)done;
}

/* ---------- inodes ---------- */

static struct enode *find_live(struct ext2_sb *s, uint32_t ino)
{
    for (struct enode *n = s->nodes; n; n = n->next) {
        if (n->vn.ino == ino)
            return n;
    }
    return NULL;
}

static int load_inode(struct superblock *sb, uint32_t ino, struct enode **out)
{
    struct ext2_sb *s = sb->priv;
    if (ino == 0 || ino > s->inodes_count)
        return -EIO;

    struct enode *live = find_live(s, ino);
    if (live) {
        live->vn.refcount++;
        *out = live;
        return 0;
    }

    uint32_t group = (ino - 1) / s->inodes_per_group;
    uint32_t index = (ino - 1) % s->inodes_per_group;
    if (group >= s->group_count)
        return -EIO;

    uint32_t gdt_per_block = s->block_size / 32;
    const uint8_t *gdt = get_block(s, s->first_data_block + 1 + group / gdt_per_block);
    if (!gdt)
        return -EIO;
    uint32_t table = le32(gdt + 32 * (group % gdt_per_block) + 8);

    uint64_t off = (uint64_t)table * s->block_size + (uint64_t)index * s->inode_size;
    uint8_t raw[256];
    if (table == 0 || table >= s->blocks_count || blk_read_bytes(s->dev, off, 128, raw) < 0)
        return -EIO;
    if (s->inode_size >= 128 && off / s->block_size >= s->blocks_count)
        return -EIO;

    struct enode *n = kzalloc(sizeof(*n));
    if (!n)
        return -ENOMEM;

    uint32_t mode = le16(raw);
    n->vn.ino = ino;
    n->vn.mode = mode;
    n->vn.nlink = le16(raw + 26);
    n->vn.size = le32(raw + 4);
    switch (mode & S_IFMT) {
    case S_IFREG:
        n->vn.type = VT_REG;
        n->vn.size |= (uint64_t)le32(raw + 108) << 32; /* i_size_high (large_file) */
        break;
    case S_IFDIR:
        n->vn.type = VT_DIR;
        break;
    case S_IFLNK:
        n->vn.type = VT_LNK;
        break;
    default:
        n->vn.type = VT_OTHER;
        break;
    }
    n->i_blocks = le32(raw + 28);
    n->file_acl = le32(raw + 104);
    for (int i = 0; i < 15; i++)
        n->block[i] = le32(raw + 40 + 4 * i);
    n->vn.sb = sb;
    n->vn.ops = &ext2_ops;
    n->vn.priv = n;
    n->vn.refcount = 1;
    n->next = s->nodes;
    s->nodes = n;
    *out = n;
    return 0;
}

static void ext2_release(struct vnode *v)
{
    struct ext2_sb *s = esb(v);
    for (struct enode **pp = &s->nodes; *pp; pp = &(*pp)->next) {
        if (&(*pp)->vn == v) {
            struct enode *n = *pp;
            *pp = n->next;
            kfree(n);
            return;
        }
    }
}

/* ---------- directories ---------- */

/*
 * Return the entry at byte offset *off of directory `d` and advance *off, skipping unused
 * slots. 1 = entry, 0 = end of directory, <0 = error (corrupt entry).
 */
static int next_entry(struct enode *d, uint64_t *off, uint32_t *ino, uint8_t *ftype, char *name,
                      size_t *name_len)
{
    struct ext2_sb *s = esb(&d->vn);
    while (*off + DIR_ENTRY_MIN <= d->vn.size) {
        uint8_t hdr[DIR_ENTRY_MIN];
        int64_t rc = file_read(d, *off, hdr, sizeof(hdr));
        if (rc < 0)
            return (int)rc;
        uint32_t rec_len = le16(hdr + 4);
        size_t nlen = hdr[6];
        uint64_t in_blk = *off % s->block_size;
        if (rec_len < DIR_ENTRY_MIN || (rec_len & 3) || in_blk + rec_len > s->block_size ||
            nlen + DIR_ENTRY_MIN > rec_len)
            return -EIO;

        uint32_t e_ino = le32(hdr);
        uint64_t here = *off;
        *off += rec_len;
        if (e_ino == 0)
            continue;

        rc = nlen ? file_read(d, here + DIR_ENTRY_MIN, name, nlen) : 0;
        if (rc < 0)
            return (int)rc;
        name[nlen] = 0;
        *ino = e_ino;
        *ftype = hdr[7];
        *name_len = nlen;
        return 1;
    }
    return 0;
}

static int ext2_lookup(struct vnode *dir, const char *name, struct vnode **out)
{
    struct enode *d = dir->priv;
    size_t want = strlen(name);
    uint64_t off = 0;
    for (;;) {
        uint32_t ino;
        uint8_t ft;
        char ename[VFS_NAME_MAX + 1];
        size_t nlen;
        int rc = next_entry(d, &off, &ino, &ft, ename, &nlen);
        if (rc < 0)
            return rc;
        if (rc == 0)
            return -ENOENT;
        if (nlen == want && memcmp(ename, name, want) == 0) {
            struct enode *n;
            rc = load_inode(dir->sb, ino, &n);
            if (rc < 0)
                return rc;
            *out = &n->vn;
            return 0;
        }
    }
}

static int ext2_readdir(struct vnode *dir, uint64_t *cookie, struct dirent *out)
{
    struct enode *d = dir->priv;
    uint32_t ino;
    uint8_t ft;
    size_t nlen;
    char name[VFS_NAME_MAX + 1];
    uint64_t off = *cookie;

    int rc = next_entry(d, &off, &ino, &ft, name, &nlen);
    if (rc <= 0)
        return rc;

    enum vtype type;
    switch (ft) {
    case 1:
        type = VT_REG;
        break;
    case 2:
        type = VT_DIR;
        break;
    case 7:
        type = VT_LNK;
        break;
    case 0: {
        struct enode *n; /* no filetype feature: ask the inode */
        rc = load_inode(dir->sb, ino, &n);
        if (rc < 0)
            return rc;
        type = n->vn.type;
        vnode_put(&n->vn);
        break;
    }
    default:
        type = VT_OTHER;
        break;
    }
    memset(out, 0, sizeof(*out));
    out->ino = ino;
    out->type = type;
    memcpy(out->name, name, nlen + 1);
    *cookie = off;
    return 1;
}

/* ---------- files and links ---------- */

static int64_t ext2_read(struct vnode *v, uint64_t off, void *buf, size_t len)
{
    return file_read(v->priv, off, buf, len);
}

static int ext2_readlink(struct vnode *v, char *buf, size_t size)
{
    struct enode *n = v->priv;
    struct ext2_sb *s = esb(v);
    uint32_t ea = n->file_acl ? s->block_size / 512 : 0;
    size_t len = v->size < size ? v->size : size;

    if (n->i_blocks == ea) { /* fast symlink: the target lives in the block pointers */
        if (v->size > 60)
            return -EIO;
        memcpy(buf, n->block, len);
        return (int)len;
    }
    if (v->size >= VFS_PATH_MAX)
        return -EIO;
    int64_t rc = file_read(n, 0, buf, len);
    return rc < 0 ? (int)rc : (int)len;
}

static const struct vnode_ops ext2_ops = {
    .lookup = ext2_lookup,
    .read = ext2_read,
    .readdir = ext2_readdir,
    .readlink = ext2_readlink,
    .release = ext2_release,
};

/* ---------- mounting ---------- */

static void free_cache(struct ext2_sb *s)
{
    for (int i = 0; i < BCACHE_ENTRIES; i++)
        kfree(s->cache[i].data);
}

static int ext2_mount(struct blockdev *dev, struct superblock **out)
{
    uint8_t raw[1024];
    if (blk_read_bytes(dev, 1024, sizeof(raw), raw) < 0)
        return -EIO;
    if (le16(raw + 56) != EXT2_MAGIC)
        return -EINVAL;

    uint32_t log = le32(raw + 24);
    uint32_t rev = le32(raw + 76);
    if (log > 4 || rev > 1)
        return -EINVAL;
    uint32_t incompat = rev ? le32(raw + 96) : 0;
    if (incompat & ~INCOMPAT_SUPPORTED) {
        printk("ext2: %s needs unsupported features (incompat %#x)\n", dev->name, incompat);
        return -EINVAL;
    }

    uint32_t bs = 1024u << log;
    uint32_t isz = rev ? le16(raw + 88) : 128;
    uint32_t blocks = le32(raw + 4);
    uint32_t first = le32(raw + 20);
    uint32_t bpg = le32(raw + 32);
    uint32_t ipg = le32(raw + 40);
    uint32_t inodes = le32(raw);

    if (isz < 128 || isz > 256 || (isz & (isz - 1)) || isz > bs || bpg == 0 || ipg == 0 ||
        blocks == 0 || first >= blocks || first != (bs == 1024 ? 1u : 0u) || inodes == 0 ||
        bpg > 8 * bs || ipg > 8 * bs || ipg * isz > bpg * bs)
        return -EINVAL;
    if ((uint64_t)blocks * bs > dev->sectors * dev->sector_size)
        return -EINVAL; /* the filesystem claims to be bigger than the device */

    uint32_t groups = (blocks - first + bpg - 1) / bpg;
    if ((uint64_t)groups * ipg < inodes)
        return -EINVAL;

    struct superblock *sb = kzalloc(sizeof(*sb));
    struct ext2_sb *s = kzalloc(sizeof(*s));
    if (!sb || !s) {
        kfree(sb);
        kfree(s);
        return -ENOMEM;
    }
    s->dev = dev;
    s->block_size = bs;
    s->blocks_count = blocks;
    s->first_data_block = first;
    s->blocks_per_group = bpg;
    s->inodes_per_group = ipg;
    s->inodes_count = inodes;
    s->inode_size = isz;
    s->group_count = groups;
    for (int i = 0; i < BCACHE_ENTRIES; i++) {
        s->cache[i].data = kmalloc(bs);
        if (!s->cache[i].data) {
            free_cache(s);
            kfree(s);
            kfree(sb);
            return -ENOMEM;
        }
    }
    sb->priv = s;
    sb->readonly = true;

    struct enode *root;
    int rc = load_inode(sb, EXT2_ROOT_INO, &root);
    if (rc == 0 && root->vn.type != VT_DIR) {
        vnode_put(&root->vn);
        rc = -EINVAL;
    }
    if (rc < 0) {
        free_cache(s);
        kfree(s);
        kfree(sb);
        return rc;
    }
    sb->root = &root->vn;
    *out = sb;
    return 0;
}

static void ext2_unmount(struct superblock *sb)
{
    struct ext2_sb *s = sb->priv;
    free_cache(s);
    kfree(s);
    kfree(sb);
}

const struct fs_type ext2_type = {
    .name = "ext2",
    .needs_device = true,
    .mount = ext2_mount,
    .unmount = ext2_unmount,
};

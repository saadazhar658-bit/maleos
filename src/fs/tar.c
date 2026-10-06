#include "fs/tar.h"

#include "fs/vfs.h"
#include "kernel/string.h"

/* ustar / GNU tar reader used to unpack the initrd into ramfs. */

#define BLOCK 512

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} __attribute__((packed));

_Static_assert(sizeof(struct tar_header) == BLOCK, "tar header must be one block");

/* Parse an octal field; -1 if it holds anything else (base-256 sizes are not supported). */
static int64_t parse_octal(const char *s, size_t n)
{
    size_t i = 0;
    while (i < n && s[i] == ' ')
        i++;
    int64_t v = 0;
    bool any = false;
    for (; i < n && s[i] >= '0' && s[i] <= '7'; i++) {
        if (v > (INT64_MAX >> 3) - 8)
            return -1;
        v = (v << 3) | (s[i] - '0');
        any = true;
    }
    for (; i < n; i++) {
        if (s[i] != ' ' && s[i] != 0)
            return -1;
    }
    return any ? v : -1;
}

static bool is_zero_block(const uint8_t *b)
{
    for (int i = 0; i < BLOCK; i++) {
        if (b[i])
            return false;
    }
    return true;
}

static bool checksum_ok(const struct tar_header *h)
{
    const uint8_t *b = (const uint8_t *)h;
    uint32_t sum = 0;
    for (size_t i = 0; i < BLOCK; i++)
        sum += (i >= 148 && i < 156) ? ' ' : b[i];
    int64_t stored = parse_octal(h->chksum, sizeof(h->chksum));
    return stored >= 0 && (uint32_t)stored == sum;
}

/* Join dest and the archive name into `out`, dropping "./" and leading slashes. */
static int make_path(char *out, const char *dest, const char *name)
{
    while (name[0] == '/' || (name[0] == '.' && name[1] == '/'))
        name += (name[0] == '/') ? 1 : 2;

    size_t dl = strlen(dest);
    while (dl > 1 && dest[dl - 1] == '/')
        dl--;
    size_t nl = strlen(name);
    while (nl > 0 && name[nl - 1] == '/')
        nl--;
    if (nl == 0 || (nl == 1 && name[0] == '.'))
        return 1; /* the archive root itself */
    if (dl + 1 + nl >= VFS_PATH_MAX)
        return -ENAMETOOLONG;

    size_t pos = 0;
    if (!(dl == 1 && dest[0] == '/')) {
        memcpy(out, dest, dl);
        pos = dl;
    }
    out[pos++] = '/';
    memcpy(out + pos, name, nl);
    out[pos + nl] = 0;
    return 0;
}

static int ensure_parent(const char *path)
{
    char buf[VFS_PATH_MAX];
    strlcpy(buf, path, sizeof(buf));
    char *slash = strrchr(buf, '/');
    if (!slash || slash == buf)
        return 0;
    *slash = 0;
    return vfs_mkdir_p(buf);
}

int tar_unpack(const void *data, size_t size, const char *dest)
{
    const uint8_t *p = data;
    size_t pos = 0;
    int count = 0;
    char longname[VFS_PATH_MAX];
    bool have_long = false;

    int rc = vfs_mkdir_p(dest);
    if (rc < 0)
        return rc;

    while (pos + BLOCK <= size) {
        const struct tar_header *h = (const struct tar_header *)(p + pos);
        if (is_zero_block(p + pos))
            break; /* end-of-archive marker */
        if (!checksum_ok(h))
            return -EINVAL;

        int64_t fsize = parse_octal(h->size, sizeof(h->size));
        if (fsize < 0)
            return -EINVAL;
        size_t body = pos + BLOCK;
        if ((uint64_t)fsize > size - body)
            return -EINVAL; /* truncated archive */
        size_t padded = ((size_t)fsize + BLOCK - 1) & ~(size_t)(BLOCK - 1);
        size_t next = body + padded;
        if (next > size && (size_t)fsize + body != size)
            return -EINVAL;
        if (next > size)
            next = size;

        char type = h->typeflag ? h->typeflag : '0';
        if (type == 'L') { /* GNU long name: the data is the name of the next entry */
            if ((size_t)fsize >= sizeof(longname))
                return -ENAMETOOLONG;
            memcpy(longname, p + body, (size_t)fsize);
            longname[fsize] = 0;
            have_long = true;
            pos = next;
            continue;
        }

        char name[VFS_PATH_MAX];
        if (have_long) {
            strlcpy(name, longname, sizeof(name));
            have_long = false;
        } else {
            char raw[256];
            size_t pl = 0;
            if (memcmp(h->magic, "ustar", 5) == 0)
                while (pl < sizeof(h->prefix) && h->prefix[pl])
                    pl++;
            size_t nl = 0;
            while (nl < sizeof(h->name) && h->name[nl])
                nl++;
            size_t o = 0;
            if (pl) {
                memcpy(raw, h->prefix, pl);
                raw[pl] = '/';
                o = pl + 1;
            }
            memcpy(raw + o, h->name, nl);
            raw[o + nl] = 0;
            strlcpy(name, raw, sizeof(name));
        }

        if (type == '0' || type == '5' || type == '2') {
            char path[VFS_PATH_MAX];
            rc = make_path(path, dest, name);
            if (rc < 0)
                return rc;
            if (rc == 0) {
                rc = ensure_parent(path);
                if (rc < 0)
                    return rc;
                if (type == '5') {
                    rc = vfs_mkdir_p(path);
                } else if (type == '2') {
                    char target[101];
                    memcpy(target, h->linkname, 100);
                    target[100] = 0;
                    vfs_unlink(path); /* extraction replaces what is there */
                    rc = vfs_symlink(target, path);
                } else {
                    int fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
                    if (fd < 0) {
                        rc = fd;
                    } else {
                        int64_t w = fsize ? vfs_write(fd, p + body, (size_t)fsize) : 0;
                        vfs_close(fd);
                        rc = (w < 0) ? (int)w : (w == fsize ? 0 : -ENOSPC);
                    }
                }
                if (rc < 0)
                    return rc;
                count++;
            }
        } /* hard links, devices, pax headers and the like are skipped */
        pos = next;
    }
    return count;
}

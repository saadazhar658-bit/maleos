#include "fs/vfs.h"

#include "fs/ext2.h"
#include "fs/ramfs.h"
#include "kernel/printk.h"
#include "kernel/string.h"
#include "kernel/sync.h"
#include "mm/heap.h"

/*
 * The virtual file system: path resolution, mounts, symbolic links and file descriptors on
 * top of the per-filesystem vnode operations.
 *
 * Locking: one mutex serializes every public vfs_* call (coarse, but simple and correct;
 * the filesystems below it need no locking of their own). Functions named *_locked or
 * static helpers assume the mutex is held. Never call a public vfs_* function from inside
 * a filesystem driver.
 *
 * Reference counting: a vnode's `refcount` counts references held by open files, by mount
 * table entries and by lookups that are still in progress. Every function that hands out a
 * vnode hands out a reference; the receiver must vnode_put() it.
 *
 * There are no processes yet, so file descriptors live in one global table and relative
 * paths are resolved from the root directory.
 */

struct mount {
    bool used;
    struct vnode *mountpoint; /* directory this is mounted on (NULL for "/") */
    struct superblock *sb;
    struct vnode *root; /* root directory of the mounted filesystem */
};

struct file {
    bool used;
    struct vnode *vn;
    uint64_t pos; /* byte offset, or the readdir cookie for directories */
    int flags;
};

static struct mutex vfs_mutex;
static struct mount mounts[VFS_MAX_MOUNTS];
static struct file files[VFS_MAX_FDS];
static const struct fs_type *fs_types[8];
static int fs_type_count;
static uint32_t next_sb_id = 1;

/* ---------- vnode references ---------- */

void vnode_get(struct vnode *v)
{
    v->refcount++;
}

void vnode_put(struct vnode *v)
{
    if (--v->refcount == 0 && v->ops->release)
        v->ops->release(v);
}

/* ---------- mounts ---------- */

static struct vnode *root_vnode(void)
{
    return mounts[0].used ? mounts[0].root : NULL;
}

static struct mount *mount_of_root(struct vnode *v)
{
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].used && mounts[i].root == v)
            return &mounts[i];
    }
    return NULL;
}

static struct mount *mount_on(struct vnode *v)
{
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].used && mounts[i].mountpoint == v)
            return &mounts[i];
    }
    return NULL;
}

int vfs_register_fs(const struct fs_type *fs)
{
    if (!fs || !fs->name || !fs->mount || fs_type_count == 8)
        return -EINVAL;
    fs_types[fs_type_count++] = fs;
    return 0;
}

static const struct fs_type *find_fs(const char *name)
{
    for (int i = 0; i < fs_type_count; i++) {
        if (strcmp(fs_types[i]->name, name) == 0)
            return fs_types[i];
    }
    return NULL;
}

/* ---------- path resolution ---------- */

static int readlink_vnode(struct vnode *v, char *buf, size_t size)
{
    if (!v->ops->readlink)
        return -EINVAL;
    return v->ops->readlink(v, buf, size);
}

/*
 * Resolve `path` starting at `start` (NULL means the root). Symbolic links met in the middle
 * of the path are always followed; the last one only if follow_last is set. On success *out
 * is a referenced vnode.
 */
static int walk(struct vnode *start, const char *path, bool follow_last, int depth,
                struct vnode **out)
{
    if (depth > VFS_MAX_SYMLINKS)
        return -ELOOP;
    if (!path[0])
        return -ENOENT;

    struct vnode *cur;
    if (path[0] == '/' || !start)
        cur = root_vnode();
    else
        cur = start;
    if (!cur)
        return -ENOENT;
    vnode_get(cur);

    const char *p = path;
    bool must_be_dir = false;

    for (;;) {
        while (*p == '/')
            p++;
        if (!*p)
            break;

        size_t n = 0;
        while (p[n] && p[n] != '/')
            n++;
        if (n > VFS_NAME_MAX) {
            vnode_put(cur);
            return -ENAMETOOLONG;
        }
        char comp[VFS_NAME_MAX + 1];
        memcpy(comp, p, n);
        comp[n] = 0;
        p += n;

        const char *q = p;
        while (*q == '/')
            q++;
        bool last = (*q == 0);
        must_be_dir = last && *p == '/';

        if (strcmp(comp, ".") == 0)
            continue;

        if (cur->type != VT_DIR) {
            vnode_put(cur);
            return -ENOTDIR;
        }

        if (strcmp(comp, "..") == 0) {
            /* Leaving the root of a mounted filesystem: continue from the directory it covers. */
            struct mount *m = mount_of_root(cur);
            if (m && m->mountpoint) {
                struct vnode *mp = m->mountpoint;
                vnode_get(mp);
                vnode_put(cur);
                cur = mp;
            }
            if (cur != root_vnode()) {
                struct vnode *parent;
                int rc = cur->ops->lookup(cur, "..", &parent);
                vnode_put(cur);
                if (rc < 0)
                    return rc;
                cur = parent;
            }
            continue;
        }

        struct vnode *child;
        int rc = cur->ops->lookup(cur, comp, &child);
        if (rc < 0) {
            vnode_put(cur);
            return rc;
        }

        struct mount *m = mount_on(child); /* entering a mounted filesystem */
        if (m) {
            struct vnode *r = m->root;
            vnode_get(r);
            vnode_put(child);
            child = r;
        }

        if (child->type == VT_LNK && (!last || follow_last || must_be_dir)) {
            char target[VFS_PATH_MAX];
            int len = readlink_vnode(child, target, sizeof(target) - 1);
            if (len < 0) {
                vnode_put(child);
                vnode_put(cur);
                return len;
            }
            target[len] = 0;

            struct vnode *resolved;
            rc = walk(cur, target, true, depth + 1, &resolved);
            vnode_put(child);
            vnode_put(cur);
            if (rc < 0)
                return rc;
            cur = resolved;
        } else {
            vnode_put(cur);
            cur = child;
        }
    }

    if (must_be_dir && cur->type != VT_DIR) {
        vnode_put(cur);
        return -ENOTDIR;
    }
    *out = cur;
    return 0;
}

/* Resolve everything but the last component. *name receives that component. */
static int walk_parent(const char *path, struct vnode **dir, char *name)
{
    size_t len = strlen(path);
    if (len == 0)
        return -ENOENT;
    if (len >= VFS_PATH_MAX)
        return -ENAMETOOLONG;

    char buf[VFS_PATH_MAX];
    memcpy(buf, path, len + 1);
    while (len > 1 && buf[len - 1] == '/')
        buf[--len] = 0;
    if (len == 1 && buf[0] == '/')
        return -EINVAL; /* the root has no parent entry to create or remove */

    char *slash = NULL;
    for (char *c = buf; *c; c++) {
        if (*c == '/')
            slash = c;
    }

    const char *dirpath;
    const char *last;
    if (!slash) {
        dirpath = "/";
        last = buf;
    } else {
        last = slash + 1;
        if (slash == buf) {
            dirpath = "/";
        } else {
            *slash = 0;
            dirpath = buf;
        }
    }

    if (strcmp(last, ".") == 0 || strcmp(last, "..") == 0)
        return -EEXIST;
    if (strlen(last) > VFS_NAME_MAX)
        return -ENAMETOOLONG;
    memcpy(name, last, strlen(last) + 1);

    int rc = walk(NULL, dirpath, true, 0, dir);
    if (rc < 0)
        return rc;
    if ((*dir)->type != VT_DIR) {
        vnode_put(*dir);
        return -ENOTDIR;
    }
    return 0;
}

static void fill_stat(const struct vnode *v, struct stat *st)
{
    st->dev = v->sb->id;
    st->ino = v->ino;
    st->mode = v->mode;
    st->nlink = v->nlink;
    st->size = v->size;
    st->blksize = 4096;
}

/* ---------- mounting ---------- */

int vfs_mount(const char *source, const char *target, const char *fstype)
{
    mutex_lock(&vfs_mutex);
    int rc = 0;
    struct vnode *mp = NULL;
    struct superblock *sb = NULL;

    const struct fs_type *fs = find_fs(fstype);
    if (!fs) {
        rc = -ENODEV;
        goto out;
    }

    struct blockdev *dev = NULL;
    if (fs->needs_device) {
        dev = source ? blk_find(source) : NULL;
        if (!dev) {
            rc = -ENODEV;
            goto out;
        }
        for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
            if (mounts[i].used && mounts[i].sb->dev == dev) {
                rc = -EBUSY; /* already mounted somewhere */
                goto out;
            }
        }
    }

    rc = walk(NULL, target, true, 0, &mp);
    if (rc < 0)
        goto out;
    if (mp->type != VT_DIR) {
        rc = -ENOTDIR;
        goto out;
    }
    if (mount_on(mp) || mount_of_root(mp)) {
        rc = -EBUSY;
        goto out;
    }

    int slot = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        rc = -ENOMEM;
        goto out;
    }

    rc = fs->mount(dev, &sb);
    if (rc < 0)
        goto out;
    sb->fs = fs;
    sb->dev = dev;
    sb->id = next_sb_id++;

    mounts[slot].used = true;
    mounts[slot].mountpoint = mp; /* the mount table keeps the reference we hold */
    mounts[slot].sb = sb;
    mounts[slot].root = sb->root;
    mp = NULL;

out:
    if (mp)
        vnode_put(mp);
    mutex_unlock(&vfs_mutex);
    return rc;
}

int vfs_umount(const char *target)
{
    mutex_lock(&vfs_mutex);
    struct vnode *v = NULL;
    int rc = walk(NULL, target, true, 0, &v);
    if (rc < 0)
        goto out;

    struct mount *m = mount_of_root(v);
    if (!m || !m->mountpoint) { /* not a mount point, or the root filesystem */
        rc = m ? -EBUSY : -EINVAL;
        goto out;
    }

    struct superblock *sb = m->sb;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (files[i].used && files[i].vn->sb == sb) {
            rc = -EBUSY;
            goto out;
        }
    }
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (mounts[i].used && mounts[i].mountpoint && mounts[i].mountpoint->sb == sb) {
            rc = -EBUSY; /* another filesystem is mounted inside this one */
            goto out;
        }
    }
    /* Our own lookup reference and the mount's reference are the only ones allowed. */
    if (m->root->refcount != 2) {
        rc = -EBUSY;
        goto out;
    }

    struct vnode *mp = m->mountpoint;
    struct vnode *root = m->root;
    m->used = false;
    vnode_put(v);    /* the reference taken by walk() above */
    vnode_put(root); /* the mount's reference */
    sb->fs->unmount(sb);
    vnode_put(mp);
    v = NULL;
    rc = 0;

out:
    if (v)
        vnode_put(v);
    mutex_unlock(&vfs_mutex);
    return rc;
}

/* ---------- file descriptors ---------- */

static struct file *fd_get(int fd)
{
    if (fd < VFS_FD_FIRST || fd >= VFS_MAX_FDS || !files[fd].used)
        return NULL;
    return &files[fd];
}

int vfs_open(const char *path, int flags)
{
    int acc = flags & O_ACCMODE;
    if (acc == O_ACCMODE || !path)
        return -EINVAL;

    mutex_lock(&vfs_mutex);
    struct vnode *v = NULL;
    int rc = walk(NULL, path, !(flags & O_NOFOLLOW), 0, &v);

    if (rc == -ENOENT && (flags & O_CREAT)) {
        struct vnode *dir;
        char name[VFS_NAME_MAX + 1];
        rc = walk_parent(path, &dir, name);
        if (rc == 0) {
            if (dir->sb->readonly)
                rc = -EROFS;
            else if (!dir->ops->create)
                rc = -ENOSYS;
            else
                rc = dir->ops->create(dir, name, VT_REG, &v);
            vnode_put(dir);
        }
    } else if (rc == 0 && (flags & O_CREAT) && (flags & O_EXCL)) {
        vnode_put(v);
        rc = -EEXIST;
    }
    if (rc < 0)
        goto out;

    if (v->type == VT_LNK) { /* only reachable with O_NOFOLLOW */
        rc = -ELOOP;
    } else if (v->type == VT_DIR) {
        if (acc != O_RDONLY)
            rc = -EISDIR;
    } else if (v->type != VT_REG) {
        rc = -ENXIO;
    } else if (flags & O_DIRECTORY) {
        rc = -ENOTDIR;
    } else if ((acc != O_RDONLY || (flags & O_TRUNC)) && v->sb->readonly) {
        rc = -EROFS;
    } else if ((flags & O_TRUNC) && acc != O_RDONLY) {
        rc = v->ops->truncate ? v->ops->truncate(v, 0) : -ENOSYS;
    }
    if (rc < 0) {
        vnode_put(v);
        goto out;
    }

    int fd = -1;
    for (int i = VFS_FD_FIRST; i < VFS_MAX_FDS; i++) {
        if (!files[i].used) {
            fd = i;
            break;
        }
    }
    if (fd < 0) {
        vnode_put(v);
        rc = -EMFILE;
        goto out;
    }
    files[fd].used = true;
    files[fd].vn = v;
    files[fd].pos = 0;
    files[fd].flags = flags;
    rc = fd;

out:
    mutex_unlock(&vfs_mutex);
    return rc;
}

int vfs_close(int fd)
{
    mutex_lock(&vfs_mutex);
    struct file *f = fd_get(fd);
    int rc = -EBADF;
    if (f) {
        struct vnode *v = f->vn;
        f->used = false;
        f->vn = NULL;
        vnode_put(v);
        rc = 0;
    }
    mutex_unlock(&vfs_mutex);
    return rc;
}

int64_t vfs_read(int fd, void *buf, size_t len)
{
    mutex_lock(&vfs_mutex);
    struct file *f = fd_get(fd);
    int64_t rc;
    if (!f || (f->flags & O_ACCMODE) == O_WRONLY) {
        rc = -EBADF;
    } else if (f->vn->type == VT_DIR) {
        rc = -EISDIR;
    } else if (!buf && len) {
        rc = -EINVAL;
    } else if (f->pos >= f->vn->size || len == 0) {
        rc = 0;
    } else {
        if (len > f->vn->size - f->pos)
            len = f->vn->size - f->pos;
        rc = f->vn->ops->read(f->vn, f->pos, buf, len);
        if (rc > 0)
            f->pos += rc;
    }
    mutex_unlock(&vfs_mutex);
    return rc;
}

int64_t vfs_write(int fd, const void *buf, size_t len)
{
    mutex_lock(&vfs_mutex);
    struct file *f = fd_get(fd);
    int64_t rc;
    if (!f || (f->flags & O_ACCMODE) == O_RDONLY) {
        rc = -EBADF;
    } else if (f->vn->type == VT_DIR) {
        rc = -EISDIR;
    } else if (!buf && len) {
        rc = -EINVAL;
    } else if (f->vn->sb->readonly) {
        rc = -EROFS;
    } else if (!f->vn->ops->write) {
        rc = -ENOSYS;
    } else if (len == 0) {
        rc = 0;
    } else {
        if (f->flags & O_APPEND)
            f->pos = f->vn->size;
        rc = f->vn->ops->write(f->vn, f->pos, buf, len);
        if (rc > 0)
            f->pos += rc;
    }
    mutex_unlock(&vfs_mutex);
    return rc;
}

int64_t vfs_lseek(int fd, int64_t offset, int whence)
{
    mutex_lock(&vfs_mutex);
    struct file *f = fd_get(fd);
    int64_t rc;
    if (!f) {
        rc = -EBADF;
    } else if (f->vn->type == VT_DIR) {
        if (whence == SEEK_SET && offset == 0) { /* rewinddir is the only seek on a directory */
            f->pos = 0;
            rc = 0;
        } else {
            rc = -ESPIPE;
        }
    } else {
        int64_t base;
        switch (whence) {
        case SEEK_SET:
            base = 0;
            break;
        case SEEK_CUR:
            base = (int64_t)f->pos;
            break;
        case SEEK_END:
            base = (int64_t)f->vn->size;
            break;
        default:
            base = -1;
            break;
        }
        if (base < 0 || (offset > 0 && base > INT64_MAX - offset) || base + offset < 0) {
            rc = -EINVAL;
        } else {
            f->pos = (uint64_t)(base + offset);
            rc = base + offset;
        }
    }
    mutex_unlock(&vfs_mutex);
    return rc;
}

int vfs_ftruncate(int fd, uint64_t size)
{
    mutex_lock(&vfs_mutex);
    struct file *f = fd_get(fd);
    int rc;
    if (!f || (f->flags & O_ACCMODE) == O_RDONLY)
        rc = -EBADF;
    else if (f->vn->type != VT_REG)
        rc = -EINVAL;
    else if (f->vn->sb->readonly)
        rc = -EROFS;
    else if (!f->vn->ops->truncate)
        rc = -ENOSYS;
    else
        rc = f->vn->ops->truncate(f->vn, size);
    mutex_unlock(&vfs_mutex);
    return rc;
}

int vfs_readdir(int fd, struct dirent *out)
{
    mutex_lock(&vfs_mutex);
    struct file *f = fd_get(fd);
    int rc;
    if (!f)
        rc = -EBADF;
    else if (f->vn->type != VT_DIR)
        rc = -ENOTDIR;
    else if (!out)
        rc = -EINVAL;
    else
        rc = f->vn->ops->readdir(f->vn, &f->pos, out);
    mutex_unlock(&vfs_mutex);
    return rc;
}

int vfs_fstat(int fd, struct stat *st)
{
    mutex_lock(&vfs_mutex);
    struct file *f = fd_get(fd);
    int rc = -EBADF;
    if (f && st) {
        fill_stat(f->vn, st);
        rc = 0;
    }
    mutex_unlock(&vfs_mutex);
    return rc;
}

static int stat_common(const char *path, struct stat *st, bool follow)
{
    if (!path || !st)
        return -EINVAL;
    mutex_lock(&vfs_mutex);
    struct vnode *v;
    int rc = walk(NULL, path, follow, 0, &v);
    if (rc == 0) {
        fill_stat(v, st);
        vnode_put(v);
    }
    mutex_unlock(&vfs_mutex);
    return rc;
}

int vfs_stat(const char *path, struct stat *st)
{
    return stat_common(path, st, true);
}

int vfs_lstat(const char *path, struct stat *st)
{
    return stat_common(path, st, false);
}

/* ---------- namespace changes ---------- */

int vfs_mkdir(const char *path)
{
    if (!path)
        return -EINVAL;
    mutex_lock(&vfs_mutex);
    struct vnode *dir;
    char name[VFS_NAME_MAX + 1];
    int rc = walk_parent(path, &dir, name);
    if (rc == 0) {
        struct vnode *existing;
        if (dir->ops->lookup(dir, name, &existing) == 0) {
            vnode_put(existing);
            rc = -EEXIST;
        } else if (dir->sb->readonly) {
            rc = -EROFS;
        } else if (!dir->ops->create) {
            rc = -ENOSYS;
        } else {
            struct vnode *nv;
            rc = dir->ops->create(dir, name, VT_DIR, &nv);
            if (rc == 0)
                vnode_put(nv);
        }
        vnode_put(dir);
    }
    mutex_unlock(&vfs_mutex);
    return rc;
}

static int remove_common(const char *path, bool want_dir)
{
    if (!path)
        return -EINVAL;
    size_t plen = strlen(path);
    if (!want_dir && plen > 1 && path[plen - 1] == '/')
        return -ENOTDIR; /* "file/" can only name a directory */
    mutex_lock(&vfs_mutex);
    struct vnode *dir;
    char name[VFS_NAME_MAX + 1];
    int rc = walk_parent(path, &dir, name);
    if (rc == 0) {
        struct vnode *child;
        rc = dir->ops->lookup(dir, name, &child);
        if (rc == 0) {
            bool is_dir = child->type == VT_DIR;
            bool busy = mount_on(child) || mount_of_root(child);
            vnode_put(child);
            if (want_dir && !is_dir)
                rc = -ENOTDIR;
            else if (!want_dir && is_dir)
                rc = -EISDIR;
            else if (busy)
                rc = -EBUSY;
            else if (dir->sb->readonly)
                rc = -EROFS;
            else if (!dir->ops->remove)
                rc = -ENOSYS;
            else
                rc = dir->ops->remove(dir, name, want_dir);
        }
        vnode_put(dir);
    }
    mutex_unlock(&vfs_mutex);
    return rc;
}

int vfs_unlink(const char *path)
{
    return remove_common(path, false);
}

int vfs_rmdir(const char *path)
{
    return remove_common(path, true);
}

int vfs_symlink(const char *target, const char *path)
{
    if (!target || !path || !target[0])
        return -EINVAL;
    if (strlen(target) >= VFS_PATH_MAX)
        return -ENAMETOOLONG;

    mutex_lock(&vfs_mutex);
    struct vnode *dir;
    char name[VFS_NAME_MAX + 1];
    int rc = walk_parent(path, &dir, name);
    if (rc == 0) {
        struct vnode *existing;
        if (dir->ops->lookup(dir, name, &existing) == 0) {
            vnode_put(existing);
            rc = -EEXIST;
        } else if (dir->sb->readonly) {
            rc = -EROFS;
        } else if (!dir->ops->symlink) {
            rc = -ENOSYS;
        } else {
            rc = dir->ops->symlink(dir, name, target);
        }
        vnode_put(dir);
    }
    mutex_unlock(&vfs_mutex);
    return rc;
}

int vfs_readlink(const char *path, char *buf, size_t size)
{
    if (!path || !buf || size == 0)
        return -EINVAL;
    mutex_lock(&vfs_mutex);
    struct vnode *v;
    int rc = walk(NULL, path, false, 0, &v);
    if (rc == 0) {
        if (v->type != VT_LNK) {
            rc = -EINVAL;
        } else {
            rc = readlink_vnode(v, buf, size);
            if (rc >= 0 && (size_t)rc < size)
                buf[rc] = 0;
        }
        vnode_put(v);
    }
    mutex_unlock(&vfs_mutex);
    return rc;
}

/* ---------- helpers ---------- */

int vfs_mkdir_p(const char *path)
{
    size_t len = strlen(path);
    if (len == 0 || len >= VFS_PATH_MAX)
        return -EINVAL;

    char buf[VFS_PATH_MAX];
    memcpy(buf, path, len + 1);
    for (size_t i = 1; i <= len; i++) {
        if (buf[i] == '/' || buf[i] == 0) {
            char saved = buf[i];
            buf[i] = 0;
            if (buf[i - 1] != '/') {
                int rc = vfs_mkdir(buf);
                if (rc < 0 && rc != -EEXIST)
                    return rc;
            }
            buf[i] = saved;
        }
    }
    return 0;
}

int64_t vfs_read_file(const char *path, void *buf, size_t max)
{
    int fd = vfs_open(path, O_RDONLY);
    if (fd < 0)
        return fd;
    uint8_t *out = buf;
    size_t total = 0;
    int64_t rc = 0;
    while (total < max) {
        rc = vfs_read(fd, out + total, max - total);
        if (rc <= 0)
            break;
        total += (size_t)rc;
    }
    vfs_close(fd);
    return rc < 0 ? rc : (int64_t)total;
}

int vfs_open_file_count(void)
{
    int n = 0;
    for (int i = VFS_FD_FIRST; i < VFS_MAX_FDS; i++)
        n += files[i].used;
    return n;
}

void vfs_dump_mounts(void)
{
    mutex_lock(&vfs_mutex);
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].used)
            continue;
        const struct superblock *sb = mounts[i].sb;
        printk("  %-6s on %-8s type %s%s\n", sb->dev ? sb->dev->name : "none",
               mounts[i].mountpoint ? "(mount)" : "/", sb->fs->name, sb->readonly ? " (ro)" : "");
    }
    mutex_unlock(&vfs_mutex);
}

void vfs_init(void)
{
    mutex_init(&vfs_mutex);
    vfs_register_fs(&ramfs_type);
    vfs_register_fs(&ext2_type);

    struct superblock *sb;
    if (ramfs_type.mount(NULL, &sb) < 0)
        kpanic("vfs: cannot create the root filesystem");
    sb->fs = &ramfs_type;
    sb->id = next_sb_id++;
    mounts[0].used = true;
    mounts[0].mountpoint = NULL;
    mounts[0].sb = sb;
    mounts[0].root = sb->root;
}

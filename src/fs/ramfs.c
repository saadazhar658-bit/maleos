#include "fs/ramfs.h"

#include "kernel/string.h"
#include "mm/heap.h"

/*
 * ramfs: a writable in-memory filesystem. It backs "/" and anything unpacked from the
 * initrd. Every node embeds its vnode. A node lives as long as it is linked into a
 * directory or something still references it; the VFS mutex serializes all access.
 */

struct rnode;

struct rentry {
    struct rentry *next;
    struct rnode *node;
    char name[]; /* NUL terminated */
};

struct rnode {
    struct vnode vn;
    struct rnode *parent; /* directories only; a removed directory points at itself */
    bool linked;          /* still reachable from a directory (the root counts as linked) */
    uint8_t *data;        /* regular files */
    size_t cap;
    struct rentry *children; /* directories */
    char *target;            /* symbolic links */
    size_t target_len;
};

static uint64_t next_ino = 1;
static const struct vnode_ops ramfs_ops;

static struct rnode *rn(struct vnode *v)
{
    return v->priv;
}

static struct rnode *node_new(struct superblock *sb, enum vtype type)
{
    struct rnode *n = kzalloc(sizeof(*n));
    if (!n)
        return NULL;
    n->vn.ino = next_ino++;
    n->vn.type = type;
    n->vn.mode = (type == VT_DIR   ? S_IFDIR | 0755
                  : type == VT_LNK ? S_IFLNK | 0777
                                   : S_IFREG | 0644);
    n->vn.nlink = type == VT_DIR ? 2 : 1;
    n->vn.sb = sb;
    n->vn.ops = &ramfs_ops;
    n->vn.priv = n;
    n->linked = true;
    return n;
}

static void node_free(struct rnode *n)
{
    kfree(n->data);
    kfree(n->target);
    kfree(n);
}

static void ramfs_release(struct vnode *v)
{
    struct rnode *n = rn(v);
    if (!n->linked)
        node_free(n);
}

/* Free a whole subtree (used when unmounting; nothing references it any more). */
static void tree_free(struct rnode *n)
{
    struct rentry *e = n->children;
    while (e) {
        struct rentry *next = e->next;
        tree_free(e->node);
        kfree(e);
        e = next;
    }
    node_free(n);
}

static struct rentry *find_entry(struct rnode *dir, const char *name, struct rentry **prev)
{
    struct rentry *p = NULL;
    for (struct rentry *e = dir->children; e; p = e, e = e->next) {
        if (strcmp(e->name, name) == 0) {
            if (prev)
                *prev = p;
            return e;
        }
    }
    return NULL;
}

static int link_child(struct rnode *dir, const char *name, struct rnode *child)
{
    struct rentry *e = kmalloc(sizeof(*e) + strlen(name) + 1);
    if (!e)
        return -ENOMEM;
    memcpy(e->name, name, strlen(name) + 1);
    e->node = child;
    e->next = dir->children;
    dir->children = e;
    if (child->vn.type == VT_DIR) {
        child->parent = dir;
        dir->vn.nlink++;
    }
    dir->vn.size++;
    return 0;
}

static int ramfs_lookup(struct vnode *dir, const char *name, struct vnode **out)
{
    struct rnode *d = rn(dir);
    struct rnode *hit;
    if (strcmp(name, ".") == 0) {
        hit = d;
    } else if (strcmp(name, "..") == 0) {
        hit = d->parent ? d->parent : d;
    } else {
        struct rentry *e = find_entry(d, name, NULL);
        if (!e)
            return -ENOENT;
        hit = e->node;
    }
    hit->vn.refcount++;
    *out = &hit->vn;
    return 0;
}

static int64_t ramfs_read(struct vnode *v, uint64_t off, void *buf, size_t len)
{
    struct rnode *n = rn(v);
    if (off >= v->size)
        return 0;
    if (len > v->size - off)
        len = v->size - off;
    memcpy(buf, n->data + off, len);
    return (int64_t)len;
}

static int ensure_capacity(struct rnode *n, uint64_t want)
{
    if (want > RAMFS_MAX_FILE)
        return -EFBIG;
    if (want <= n->cap)
        return 0;
    size_t cap = n->cap ? n->cap : 64;
    while (cap < want)
        cap *= 2;
    uint8_t *p = kzalloc(cap);
    if (!p)
        return -ENOMEM;
    if (n->data)
        memcpy(p, n->data, n->vn.size);
    kfree(n->data);
    n->data = p;
    n->cap = cap;
    return 0;
}

static int64_t ramfs_write(struct vnode *v, uint64_t off, const void *buf, size_t len)
{
    struct rnode *n = rn(v);
    if (off > RAMFS_MAX_FILE || len > RAMFS_MAX_FILE - off)
        return -EFBIG;
    int rc = ensure_capacity(n, off + len);
    if (rc < 0)
        return rc;
    if (off > v->size) /* sparse write: the gap reads as zeros (kzalloc / truncate keep it so) */
        memset(n->data + v->size, 0, off - v->size);
    memcpy(n->data + off, buf, len);
    if (off + len > v->size)
        v->size = off + len;
    return (int64_t)len;
}

static int ramfs_truncate(struct vnode *v, uint64_t size)
{
    struct rnode *n = rn(v);
    int rc = ensure_capacity(n, size);
    if (rc < 0)
        return rc;
    if (size > v->size)
        memset(n->data + v->size, 0, size - v->size);
    v->size = size;
    return 0;
}

static int ramfs_readdir(struct vnode *dir, uint64_t *cookie, struct dirent *out)
{
    struct rnode *d = rn(dir);
    uint64_t idx = *cookie;
    memset(out, 0, sizeof(*out));

    if (idx == 0 || idx == 1) {
        out->ino = idx == 0 ? d->vn.ino : (d->parent ? d->parent->vn.ino : d->vn.ino);
        out->type = VT_DIR;
        strlcpy(out->name, idx == 0 ? "." : "..", sizeof(out->name));
        *cookie = idx + 1;
        return 1;
    }
    uint64_t want = idx - 2;
    struct rentry *e = d->children;
    /* children are prepended, so walk from the back to list them in creation order */
    uint64_t total = d->vn.size;
    if (want >= total)
        return 0;
    uint64_t from_head = total - 1 - want;
    while (from_head-- && e)
        e = e->next;
    if (!e)
        return 0;
    out->ino = e->node->vn.ino;
    out->type = e->node->vn.type;
    strlcpy(out->name, e->name, sizeof(out->name));
    *cookie = idx + 1;
    return 1;
}

static int ramfs_create(struct vnode *dir, const char *name, enum vtype type, struct vnode **out)
{
    struct rnode *d = rn(dir);
    if (type != VT_REG && type != VT_DIR)
        return -EINVAL;
    if (find_entry(d, name, NULL))
        return -EEXIST;
    struct rnode *n = node_new(dir->sb, type);
    if (!n)
        return -ENOMEM;
    int rc = link_child(d, name, n);
    if (rc < 0) {
        node_free(n);
        return rc;
    }
    n->vn.refcount = 1;
    *out = &n->vn;
    return 0;
}

static int ramfs_symlink(struct vnode *dir, const char *name, const char *target)
{
    struct rnode *d = rn(dir);
    if (find_entry(d, name, NULL))
        return -EEXIST;
    struct rnode *n = node_new(dir->sb, VT_LNK);
    if (!n)
        return -ENOMEM;
    n->target_len = strlen(target);
    n->target = kmalloc(n->target_len + 1);
    if (!n->target) {
        node_free(n);
        return -ENOMEM;
    }
    memcpy(n->target, target, n->target_len + 1);
    n->vn.size = n->target_len;
    int rc = link_child(d, name, n);
    if (rc < 0)
        node_free(n);
    return rc;
}

static int ramfs_remove(struct vnode *dir, const char *name, bool is_dir)
{
    struct rnode *d = rn(dir);
    struct rentry *prev;
    struct rentry *e = find_entry(d, name, &prev);
    if (!e)
        return -ENOENT;
    struct rnode *n = e->node;
    if (is_dir && n->children)
        return -ENOTEMPTY;

    if (prev)
        prev->next = e->next;
    else
        d->children = e->next;
    kfree(e);
    d->vn.size--;
    if (n->vn.type == VT_DIR) {
        d->vn.nlink--;
        n->parent = n;
    }
    n->linked = false;
    n->vn.nlink = 0;
    if (n->vn.refcount == 0)
        node_free(n);
    return 0;
}

static int ramfs_readlink(struct vnode *v, char *buf, size_t size)
{
    struct rnode *n = rn(v);
    size_t len = n->target_len < size ? n->target_len : size;
    memcpy(buf, n->target, len);
    return (int)len;
}

static const struct vnode_ops ramfs_ops = {
    .lookup = ramfs_lookup,
    .read = ramfs_read,
    .write = ramfs_write,
    .readdir = ramfs_readdir,
    .create = ramfs_create,
    .symlink = ramfs_symlink,
    .remove = ramfs_remove,
    .truncate = ramfs_truncate,
    .readlink = ramfs_readlink,
    .release = ramfs_release,
};

static int ramfs_mount(struct blockdev *dev, struct superblock **out)
{
    (void)dev;
    struct superblock *sb = kzalloc(sizeof(*sb));
    if (!sb)
        return -ENOMEM;
    struct rnode *root = node_new(sb, VT_DIR);
    if (!root) {
        kfree(sb);
        return -ENOMEM;
    }
    root->parent = root;
    root->vn.refcount = 1; /* the mount's reference */
    sb->root = &root->vn;
    *out = sb;
    return 0;
}

static void ramfs_unmount(struct superblock *sb)
{
    tree_free(rn(sb->root));
    kfree(sb);
}

const struct fs_type ramfs_type = {
    .name = "ramfs",
    .needs_device = false,
    .mount = ramfs_mount,
    .unmount = ramfs_unmount,
};

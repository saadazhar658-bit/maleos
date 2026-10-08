#ifndef MALEOS_FS_VFS_H
#define MALEOS_FS_VFS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "abi/abi.h"
#include "drivers/block.h"
#include "kernel/errno.h"

#define VFS_MAX_FDS 64
#define VFS_FD_FIRST 3 /* 0-2 are kept free for stdin/stdout/stderr */
#define VFS_MAX_MOUNTS 8
#define VFS_MAX_SYMLINKS 8

/* ---- filesystem driver interface ---- */

struct vnode;
struct superblock;

struct vnode_ops {
    /* Find `name` in directory `dir`. On success *out is a new reference. */
    int (*lookup)(struct vnode *dir, const char *name, struct vnode **out);
    /* Return bytes read (0 at EOF) or -errno. The VFS has already clipped the range to size. */
    int64_t (*read)(struct vnode *v, uint64_t off, void *buf, size_t len);
    /* Return bytes written or -errno. NULL on read-only filesystems. */
    int64_t (*write)(struct vnode *v, uint64_t off, const void *buf, size_t len);
    /* *cookie is opaque and starts at 0. Returns 1 and fills *out, 0 at the end, or -errno. */
    int (*readdir)(struct vnode *dir, uint64_t *cookie, struct dirent *out);
    int (*create)(struct vnode *dir, const char *name, enum vtype type, struct vnode **out);
    int (*symlink)(struct vnode *dir, const char *name, const char *target);
    int (*remove)(struct vnode *dir, const char *name, bool is_dir);
    int (*truncate)(struct vnode *v, uint64_t size);
    /* Return the target length (not NUL terminated, at most `size`) or -errno. */
    int (*readlink)(struct vnode *v, char *buf, size_t size);
    /* The last reference was dropped. */
    void (*release)(struct vnode *v);
};

struct vnode {
    uint64_t ino;
    enum vtype type;
    uint64_t size;
    uint32_t mode;
    uint32_t nlink;
    int refcount; /* references held by open files, mounts and in-flight lookups */
    struct superblock *sb;
    const struct vnode_ops *ops;
    void *priv;
};

struct fs_type {
    const char *name;
    bool needs_device;
    int (*mount)(struct blockdev *dev, struct superblock **out);
    void (*unmount)(struct superblock *sb);
};

struct superblock {
    const struct fs_type *fs;
    struct vnode *root;
    struct blockdev *dev;
    bool readonly;
    uint32_t id;
    void *priv;
};

void vnode_get(struct vnode *v);
void vnode_put(struct vnode *v);

int vfs_register_fs(const struct fs_type *fs);

/* ---- the API the rest of the kernel (and later, system calls) uses ---- */

void vfs_init(void); /* register built-in filesystems and mount an empty ramfs as "/" */

int vfs_mount(const char *source, const char *target, const char *fstype);
int vfs_umount(const char *target);

int vfs_open(const char *path, int flags);
int vfs_close(int fd);
int64_t vfs_read(int fd, void *buf, size_t len);
int64_t vfs_write(int fd, const void *buf, size_t len);
int64_t vfs_lseek(int fd, int64_t offset, int whence);
int vfs_ftruncate(int fd, uint64_t size);
int vfs_readdir(int fd, struct dirent *out); /* 1 = entry, 0 = end, <0 = error */

int vfs_stat(const char *path, struct stat *st);  /* follows symbolic links */
int vfs_lstat(const char *path, struct stat *st); /* does not follow the last link */
int vfs_fstat(int fd, struct stat *st);

int vfs_mkdir(const char *path);
int vfs_rmdir(const char *path);
int vfs_unlink(const char *path);
int vfs_symlink(const char *target, const char *path);
int vfs_readlink(const char *path, char *buf,
                 size_t size); /* returns length, NUL-terminated if room */

/* Convenience helpers. */
int vfs_mkdir_p(const char *path);
int64_t vfs_read_file(const char *path, void *buf, size_t max); /* whole file, or -errno */

int vfs_open_file_count(void);
void vfs_dump_mounts(void);

#endif

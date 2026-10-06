#ifndef MALEOS_FS_RAMFS_H
#define MALEOS_FS_RAMFS_H

#include "fs/vfs.h"

#define RAMFS_MAX_FILE (32ULL << 20)

extern const struct fs_type ramfs_type;

#endif

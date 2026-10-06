#ifndef MALEOS_FS_EXT2_H
#define MALEOS_FS_EXT2_H

#include "fs/vfs.h"

/*
 * Read-only ext2 driver. Supports 1-16 KiB blocks, revision 0 and 1, direct, single, double
 * and triple indirect blocks, sparse files, fast and slow symbolic links. Filesystems that
 * need features it does not implement (journal replay, extents, 64-bit, ...) are refused.
 */
extern const struct fs_type ext2_type;

#endif

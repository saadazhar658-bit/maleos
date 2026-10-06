#ifndef MALEOS_FS_TAR_H
#define MALEOS_FS_TAR_H

#include <stddef.h>

/*
 * Unpack a ustar archive into the VFS below `dest` (e.g. "/" or "/tmp/x"). Handles
 * directories, regular files, symbolic links and GNU long names; missing parent directories
 * are created. Returns the number of entries extracted or -errno (-EINVAL for a corrupt
 * archive: bad checksum or truncated data).
 */
int tar_unpack(const void *data, size_t size, const char *dest);

#endif

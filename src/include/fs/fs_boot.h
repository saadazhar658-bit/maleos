#ifndef MALEOS_FS_FS_BOOT_H
#define MALEOS_FS_FS_BOOT_H

#include "kernel/bootinfo.h"

/*
 * Unpack the initrd module (if any) into "/", create /tmp, /mnt and /mnt2 and mount the
 * first two disks on /mnt and /mnt2 when they hold an ext2 filesystem. Call after vfs_init()
 * and driver probing.
 */
void fs_boot_init(const struct boot_info *bi);

/* Filesystem checks; returns the number of checks (panics on failure). */
int fs_selftest(void);

#endif

#ifndef MALEOS_KERNEL_USERLAND_SELFTEST_H
#define MALEOS_KERNEL_USERLAND_SELFTEST_H

/*
 * Runs the programs in /tests, feeds malformed ELF files to the loader, and checks that no
 * frames, heap memory, descriptors or process slots leak. Returns the number of checks
 * (panics on failure). Needs the initrd, so it runs after fs_boot_init().
 */
int userland_selftest(void);

#endif

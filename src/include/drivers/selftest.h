#ifndef MALEOS_DRIVERS_SELFTEST_H
#define MALEOS_DRIVERS_SELFTEST_H

/* PS/2 decoder, PCI, I/O APIC and (when disks are present) storage checks. */
int drivers_selftest(void);

/*
 * Block layer, ATA and AHCI checks against the attached test disks. Returns the number of
 * checks, or -1 if no disks are attached (the tests are skipped).
 */
int storage_selftest(void);

#endif

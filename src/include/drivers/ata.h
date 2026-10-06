#ifndef MALEOS_DRIVERS_ATA_H
#define MALEOS_DRIVERS_ATA_H

#include "drivers/driver.h"

extern const struct driver ata_driver;

/*
 * Test hook: when set, drives that support LBA48 are driven with the older 28-bit commands
 * instead (valid only below 128 GiB), so both command sets can be exercised.
 */
extern int ata_force_lba28;

#endif

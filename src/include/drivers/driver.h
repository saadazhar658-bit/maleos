#ifndef MALEOS_DRIVERS_DRIVER_H
#define MALEOS_DRIVERS_DRIVER_H

#include <stdint.h>

#include "drivers/pci.h"

/*
 * Driver shim layer: the one stable interface between drivers and the rest of the kernel.
 *
 * A driver describes itself with a struct driver and registers it. Two kinds exist:
 *   - PCI drivers list `match` entries and implement probe_pci(); they are offered every
 *     unclaimed PCI device that matches.
 *   - Platform drivers (fixed hardware such as the PS/2 controller) implement init().
 *
 * Both return 0 on success. A driver that does not want a device returns a negative value
 * and the device stays available for the next candidate.
 */

#define MATCH_VENDOR (1u << 0)
#define MATCH_DEVICE (1u << 1)
#define MATCH_CLASS (1u << 2)
#define MATCH_SUBCLASS (1u << 3)
#define MATCH_PROG_IF (1u << 4)

struct pci_match {
    uint32_t flags; /* which of the fields below are significant */
    uint16_t vendor, device;
    uint8_t class_code, subclass, prog_if;
};

struct driver {
    const char *name;
    const struct pci_match *match; /* array terminated by an entry with flags == 0 */
    int (*probe_pci)(struct pci_dev *dev);
    int (*init)(void);
};

#define DRIVER_MAX 16

int driver_register(const struct driver *drv);

/* Run init() of every platform driver, then offer every PCI device to the PCI drivers. */
void drivers_probe_all(void);

int driver_count(void);
const struct driver *driver_at(int index);

/* Registers all built-in drivers (drivers/builtin.c). */
void drivers_register_builtin(void);

#endif

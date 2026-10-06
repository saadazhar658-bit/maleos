#include "drivers/driver.h"

#include <stddef.h>

#include "kernel/printk.h"

static const struct driver *drivers[DRIVER_MAX];
static int count;

int driver_register(const struct driver *drv)
{
    if (!drv || !drv->name || (!drv->probe_pci && !drv->init))
        return -1;
    if (count == DRIVER_MAX)
        return -1;
    drivers[count++] = drv;
    return 0;
}

int driver_count(void)
{
    return count;
}

const struct driver *driver_at(int index)
{
    return (index >= 0 && index < count) ? drivers[index] : NULL;
}

static bool matches(const struct pci_match *m, const struct pci_dev *d)
{
    if (!m)
        return false;
    for (; m->flags; m++) {
        if ((m->flags & MATCH_VENDOR) && m->vendor != d->vendor)
            continue;
        if ((m->flags & MATCH_DEVICE) && m->device != d->device)
            continue;
        if ((m->flags & MATCH_CLASS) && m->class_code != d->class_code)
            continue;
        if ((m->flags & MATCH_SUBCLASS) && m->subclass != d->subclass)
            continue;
        if ((m->flags & MATCH_PROG_IF) && m->prog_if != d->prog_if)
            continue;
        return true;
    }
    return false;
}

void drivers_probe_all(void)
{
    for (int i = 0; i < count; i++) {
        if (drivers[i]->init) {
            int rc = drivers[i]->init();
            if (rc != 0)
                printk("driver %s: init failed (%d)\n", drivers[i]->name, rc);
        }
    }

    for (int n = 0; n < pci_device_count(); n++) {
        struct pci_dev *d = pci_device(n);
        for (int i = 0; i < count && !d->bound; i++) {
            if (!drivers[i]->probe_pci || !matches(drivers[i]->match, d))
                continue;
            if (drivers[i]->probe_pci(d) == 0) {
                d->bound = true;
                d->owner = drivers[i]->name;
            }
        }
    }
}

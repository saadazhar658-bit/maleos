#ifndef MALEOS_DRIVERS_E1000_H
#define MALEOS_DRIVERS_E1000_H

#include "drivers/driver.h"

/* Intel 8254x ("e1000") gigabit Ethernet, the card QEMU emulates with `-device e1000`. */
extern const struct driver e1000_driver;

#endif

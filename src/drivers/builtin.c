#include "drivers/ahci.h"
#include "drivers/ata.h"
#include "drivers/driver.h"
#include "drivers/kbd.h"

void drivers_register_builtin(void)
{
    driver_register(&kbd_driver);
    driver_register(&ata_driver);
    driver_register(&ahci_driver);
}

#include "arch/power.h"

#include "arch/cpu.h"
#include "kernel/printk.h"

void machine_poweroff(void)
{
    printk("System halted: powering off\n");
    outw(0x604, 0x2000);  /* QEMU (ACPI PM) */
    outw(0xB004, 0x2000); /* older QEMU / Bochs */
    halt_forever();
}

#ifndef MALEOS_ARCH_POWER_H
#define MALEOS_ARCH_POWER_H

/* Ask the machine to switch off (QEMU/ACPI); halts the CPU if that has no effect. */
__attribute__((noreturn)) void machine_poweroff(void);

#endif

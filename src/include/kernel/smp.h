#ifndef MALEOS_KERNEL_SMP_H
#define MALEOS_KERNEL_SMP_H

#include "arch/percpu.h"

/* Start every other CPU the MADT lists. Returns how many CPUs started (including the boot CPU). */
int smp_init(void);
int smp_online_count(void);

#endif

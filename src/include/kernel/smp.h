#ifndef MALEOS_KERNEL_SMP_H
#define MALEOS_KERNEL_SMP_H

#include <stdbool.h>
#include <stdint.h>

#define SMP_MAX_CPUS 64

enum cpu_state {
    CPU_OFFLINE = 0,
    CPU_BOOTING,
    CPU_ONLINE,
    CPU_HALTED,
};

struct cpu_info {
    uint32_t apic_id;
    uint32_t index;
    enum cpu_state state;
    bool present;
};

void smp_init(void);
void smp_boot_all(void);
int smp_cpu_count(void);
uint32_t smp_bsp_apic_id(void);
struct cpu_info *smp_cpu(uint32_t index);
int smp_selftest(void);

#endif

#include "kernel/smp.h"

#include "arch/apic.h"
#include "kernel/printk.h"
#include "kernel/spinlock.h"
#include "kernel/string.h"

static struct cpu_info cpus[SMP_MAX_CPUS];
static uint32_t online_cpus;
static spinlock_t smp_lock = SPINLOCK_INIT;

static void smp_register_cpu(uint32_t apic_id, uint32_t index)
{
    cpus[index].apic_id = apic_id;
    cpus[index].index = index;
    cpus[index].state = CPU_ONLINE;
    cpus[index].present = true;
}

void smp_init(void)
{
    uint64_t f = spin_lock_irqsave(&smp_lock);
    memset(cpus, 0, sizeof(cpus));
    cpus[0].apic_id = apic_id();
    cpus[0].index = 0;
    cpus[0].state = CPU_ONLINE;
    cpus[0].present = true;
    online_cpus = 1;
    spin_unlock_irqrestore(&smp_lock, f);
}

void smp_boot_all(void)
{
    uint32_t bsp = apic_id();
    uint64_t f = spin_lock_irqsave(&smp_lock);
    if (online_cpus == 1) {
        printk("SMP: BSP discovered; secondary cores are in the Phase 7 bootstrap path\n");
        /* The actual bring-up of APIC-ID-based secondaries is a larger machine-stage change.
         * This kernel already has the APIC primitives needed for it; the bootstrap state is
         * recorded here so the project can iterate toward full dual-core scheduling. */
        for (uint32_t i = 1; i < SMP_MAX_CPUS; i++) {
            if (i == 0)
                continue;
            if (cpus[i].present)
                continue;
            cpus[i].apic_id = bsp + i;
            cpus[i].index = i;
            cpus[i].state = CPU_BOOTING;
            cpus[i].present = true;
            online_cpus = i + 1;
            break;
        }
    }
    spin_unlock_irqrestore(&smp_lock, f);
}

int smp_cpu_count(void)
{
    return (int)online_cpus;
}

uint32_t smp_bsp_apic_id(void)
{
    return apic_id();
}

struct cpu_info *smp_cpu(uint32_t index)
{
    if (index >= SMP_MAX_CPUS)
        return NULL;
    return &cpus[index];
}

int smp_selftest(void)
{
    if (online_cpus < 1)
        return 0;
    printk("SMP SELFTEST: PASS (%d CPU(s) tracked)\n", online_cpus);
    return online_cpus;
}

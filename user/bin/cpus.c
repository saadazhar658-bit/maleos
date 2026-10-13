#include "ulib.h"

/* Show the CPUs the scheduler uses and what each has been doing since boot. */
int main(void)
{
    struct abi_cpuinfo ci;
    if (cpuinfo(&ci) < 0) {
        puts("cpus: cannot read CPU information");
        return 1;
    }
    printf("%u CPU(s) running threads; this call ran on CPU %u\n", ci.ncpus, ci.cpu);
    printf("cpu  ticks     idle      user\n");
    for (unsigned i = 0; i < ci.ncpus && i < ABI_CPU_MAX; i++)
        printf("%3u  %-8lu  %-8lu  %lu\n", i, (unsigned long)ci.ticks[i],
               (unsigned long)ci.idle_ticks[i], (unsigned long)ci.user_ticks[i]);
    return 0;
}

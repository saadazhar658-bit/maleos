#include "ulib.h"

int main(void)
{
    struct abi_procinfo info[32];
    long n = procinfo(info, 32);
    printf("  PID  PPID STATE    PAGES   TICKS NAME\n");
    for (long i = 0; i < n; i++)
        printf("%5u %5u %-7s %6u %7lu %s\n", info[i].pid, info[i].ppid,
               info[i].state == PROC_RUNNING ? "running" : "zombie", info[i].pages,
               (unsigned long)info[i].cpu_ticks, info[i].name);
    return 0;
}

#include "ulib.h"

/*
 * Do user programs really run on several CPUs at once? Start one busy worker per CPU and
 * compare the user-mode timer ticks all CPUs saw with the wall-clock ticks that passed:
 * on one CPU the ratio cannot exceed 1, with N CPUs working it approaches N.
 *
 *   t_smp           parent: runs the check, exit 0 when it holds (or there is one CPU)
 *   t_smp worker    spins in user mode for about 300 ms, exits with 1 + the CPU it ended on
 */
#define SPIN_MS 300

static volatile unsigned long sink; /* volatile: the compiler must not fold the busy loop away */

int main(int argc, char **argv)
{
    (void)argv;
    struct abi_cpuinfo ci;
    if (cpuinfo(&ci) < 0)
        return 1;

    if (argc > 1) {
        unsigned long end = uptime_ms() + SPIN_MS;
        while (uptime_ms() < end) {
            for (int i = 0; i < 200000; i++)
                sink += (unsigned long)i * 7; /* long stretch of user-mode work between clock reads
                                                 (each read is a system call) */
        }
        cpuinfo(&ci);
        return 1 + (int)ci.cpu;
    }

    unsigned n = ci.ncpus;
    if (n < 2) {
        printf("t_smp: one CPU, nothing to compare\n");
        return 0;
    }
    if (n > ABI_CPU_MAX)
        n = ABI_CPU_MAX;

    unsigned long user_before = 0;
    for (unsigned i = 0; i < ABI_CPU_MAX; i++)
        user_before += ci.user_ticks[i];
    unsigned long t0 = uptime_ms();

    int pids[ABI_CPU_MAX];
    for (unsigned i = 0; i < n; i++) {
        char *wargv[] = {"t_smp", "worker", NULL};
        long pid = spawn("/tests/t_smp", wargv, NULL);
        if (pid < 0)
            return 2;
        pids[i] = (int)pid;
    }

    unsigned seen = 0;
    for (unsigned i = 0; i < n; i++) {
        int status = -1;
        if (wait(pids[i], &status) != pids[i] || status < 1 || status > ABI_CPU_MAX)
            return 3;
        seen |= 1u << (status - 1);
    }

    unsigned long elapsed = uptime_ms() - t0;
    cpuinfo(&ci);
    unsigned long user_after = 0;
    for (unsigned i = 0; i < ABI_CPU_MAX; i++)
        user_after += ci.user_ticks[i];

    unsigned long wall_ticks = elapsed / 10;
    unsigned long user_ticks = user_after - user_before;
    int cpus_seen = 0;
    for (unsigned v = seen; v; v &= v - 1)
        cpus_seen++;

    printf("t_smp: %u CPUs, %lu user ticks in %lu wall ticks, workers ended on %d CPU(s)\n", n,
           user_ticks, wall_ticks, cpus_seen);

    /* Parallel work must show up as clearly more user ticks than wall ticks. */
    if (user_ticks * 2 < wall_ticks * 3)
        return 4;
    if (cpus_seen < 2)
        return 5;
    return 0;
}

#include "kernel/smp_selftest.h"

#include "arch/percpu.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/smp.h"
#include "mm/heap.h"

static int checks;

#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond))                                                                               \
            kpanic("smp selftest failed: %s (%s:%d)", what, __FILE__, __LINE__);                   \
    } while (0)

static volatile uint32_t seen_mask;
static volatile uint32_t hops;

static int bits_set(uint32_t v)
{
    int n = 0;
    for (; v; v &= v - 1)
        n++;
    return n;
}

static void hopper(void *arg)
{
    /* Staggered sleeps, so wake-ups land in different ticks: with the big kernel lock a CPU
     * that is already running kernel code takes every thread that is ready at that moment. */
    uint64_t period = 20 + (uint64_t)arg * 10;
    for (int i = 0; i < 25; i++) {
        __atomic_fetch_or(&seen_mask, 1u << this_cpu()->id, __ATOMIC_RELAXED);
        __atomic_fetch_add(&hops, 1, __ATOMIC_RELAXED);
        thread_sleep_ms(period);
    }
}

static void quick(void *arg)
{
    /* Returns a value derived from its argument and the CPU it ran on being a valid one. */
    uint64_t v = (uint64_t)arg;
    thread_exit((int)(v * 3 + (this_cpu()->id < SMP_MAX ? 0 : 1000)));
}

int smp_selftest(int ncpu)
{
    checks = 0;

    /* The APs take the kernel lock only when this CPU lets go of it; sleeping does that. */
    for (int i = 0; i < 300 && smp_online_count() < ncpu; i++)
        thread_sleep_ms(10);
    CHECK(smp_online_count() == ncpu, "every started CPU came online");
    CHECK(cpu_count == ncpu, "cpu_count matches");
    CHECK(this_cpu()->id < SMP_MAX && this_cpu() == &cpus[this_cpu()->id],
          "this_cpu() points at our slot");
    CHECK(bkl_held(), "kernel code runs under the big kernel lock");

    for (int i = 0; i < ncpu; i++) {
        CHECK(cpus[i].online && cpus[i].id == (uint32_t)i, "cpu slot is consistent");
        for (int j = i + 1; j < ncpu; j++)
            CHECK(cpus[i].apic_id != cpus[j].apic_id, "APIC ids are unique");
    }

    sched_set_cpu_limit(ncpu);
    seen_mask = 0;
    hops = 0;
    uint64_t t0 = timer_ticks();
    uint64_t ticks0[SMP_MAX];
    for (int i = 0; i < ncpu; i++)
        ticks0[i] = cpus[i].ticks;

    /* Threads that sleep and wake in the same ticks. With one big kernel lock the CPU that
     * holds it takes whatever is ready, so they rarely spread out; none may run twice or get
     * lost, and the idle CPUs must be woken by IPI. */
    struct thread *th[2 * SMP_MAX];
    int nth = 2 * ncpu;
    for (int i = 0; i < nth; i++) {
        th[i] = thread_create("hopper", hopper, (void *)(uint64_t)(i % 3), SCHED_PRIO_NORMAL);
        CHECK(th[i] != NULL, "create hopper");
    }
    for (int i = 0; i < nth; i++)
        thread_join(th[i]);

    CHECK(hops == (uint32_t)nth * 25, "every hop of every thread happened exactly once");
    int cpus_used = bits_set(seen_mask);
    CHECK(cpus_used >= 1, "threads ran");

    /* Every CPU's own timer kept ticking at about the same rate as the clock. */
    uint64_t dt = timer_ticks() - t0;
    for (int i = 0; i < ncpu; i++) {
        uint64_t d = cpus[i].ticks - ticks0[i];
        CHECK(d + 5 >= dt && d <= dt + 5, "per-CPU timer rate matches the clock");
    }

    uint64_t ipis = 0;
    for (int i = 0; i < ncpu; i++)
        ipis += cpus[i].ipis;
    if (ncpu > 1)
        CHECK(ipis > 0, "reschedule IPIs were delivered");

    /* Create and reap many threads while other CPUs run: kernel stacks are mapped, used on
     * several CPUs, unmapped and reused, which exercises the cross-CPU TLB handling. */
    struct heap_stats h0, h1;
    heap_get_stats(&h0);
    for (int round = 0; round < 40; round++) {
        struct thread *q[6];
        for (int i = 0; i < 6; i++) {
            q[i] =
                thread_create("quick", quick, (void *)(uint64_t)(round * 6 + i), SCHED_PRIO_NORMAL);
            CHECK(q[i] != NULL, "create quick");
        }
        for (int i = 0; i < 6; i++)
            CHECK(thread_join(q[i]) == (round * 6 + i) * 3, "thread result survived the CPU hop");
    }
    CHECK(heap_check() == 0, "heap consistent after multi-CPU thread churn");
    heap_get_stats(&h1);
    CHECK(h1.used_bytes <= h0.used_bytes + 4096, "no heap growth from thread churn");

    printk("SMP: threads ran on %d CPU(s), %lu reschedule IPI(s)\n", cpus_used,
           (unsigned long)ipis);
    return checks;
}

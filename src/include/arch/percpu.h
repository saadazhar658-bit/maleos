#ifndef MALEOS_ARCH_PERCPU_H
#define MALEOS_ARCH_PERCPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SMP_MAX 8
#define AP_STACK_SIZE 16384

struct thread;

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

/*
 * Per-CPU data. The CPU's GS base points at its own struct, so `gs:0` is always "me". The
 * fields the assembly entry code touches sit at fixed offsets (checked below).
 */
struct cpu {
    struct cpu *self;    /* 0 */
    uint64_t kstack_top; /* 8:  kernel stack the SYSCALL entry switches to */
    uint64_t user_rsp;   /* 16: scratch for the SYSCALL entry */
    uint32_t id;         /* 24: index into cpus[] (0 = boot CPU) */
    uint32_t apic_id;    /* 28 */
    struct thread *current;
    struct thread *idle;
    volatile int irq_nest; /* >0 while handling an interrupt */
    volatile bool resched; /* reschedule requested for this CPU */
    volatile bool started; /* finished its private bring-up (AP) */
    volatile bool online;  /* running the scheduler */
    uint64_t tlb_seen;     /* last kernel TLB generation this CPU has flushed for */
    uint64_t ticks;        /* timer ticks seen */
    uint64_t idle_ticks;   /* ... with the idle thread running */
    uint64_t user_ticks;   /* ... interrupting user mode */
    uint64_t ipis;         /* reschedule IPIs received */
    uint64_t picks;        /* threads dispatched here (not counting idle) */
    uint32_t stall;        /* consecutive ticks idle with work waiting (safety net) */
    uint64_t gdt[8] __attribute__((aligned(16)));
    struct tss tss;
    uint8_t df_stack[4096] __attribute__((aligned(16)));
};

#define CPU_OFF_KSTACK_TOP 8
#define CPU_OFF_USER_RSP 16
_Static_assert(offsetof(struct cpu, kstack_top) == CPU_OFF_KSTACK_TOP, "asm offset");
_Static_assert(offsetof(struct cpu, user_rsp) == CPU_OFF_USER_RSP, "asm offset");

extern struct cpu cpus[SMP_MAX];
extern int cpu_count; /* CPUs that finished bring-up (including the boot CPU) */

static inline struct cpu *this_cpu(void)
{
    struct cpu *c;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(c));
    return c;
}

/* ---- the big kernel lock ----
 * Kernel code runs on one CPU at a time. A CPU takes the lock on every entry from user mode
 * (interrupt, exception, SYSCALL) and drops it on the way back out, and around `hlt` in the
 * idle loop. Switching threads on a CPU keeps the lock; it is a property of the CPU's
 * kernel-mode execution, not of a thread.
 */
void bkl_lock(void);
void bkl_unlock(void);
bool bkl_held(void);

/* True if the interrupted context was running without the big kernel lock. */
bool smp_frame_unlocked(uint64_t cs, uint64_t rip);

/* Park the CPU until an interrupt: releases the lock around `hlt`. Called with it held. */
void cpu_idle_wait(void);

/* Kernel mappings were removed: every CPU flushes before it next runs kernel code. */
void smp_tlb_bump(void);

#endif

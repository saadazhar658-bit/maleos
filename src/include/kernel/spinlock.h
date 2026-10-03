#ifndef MALEOS_KERNEL_SPINLOCK_H
#define MALEOS_KERNEL_SPINLOCK_H

#include <stdbool.h>
#include <stdint.h>

#define RFLAGS_IF (1ULL << 9)

/*
 * Interrupt flag helpers.
 *
 * irq_save()         disable interrupts, return the previous RFLAGS
 * irq_restore()      restore RFLAGS; if interrupts come back on, honour a pending reschedule
 * irq_restore_raw()  restore without the reschedule check (scheduler internals only)
 */
static inline uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline bool irq_enabled(void)
{
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
    return flags & RFLAGS_IF;
}

static inline void irq_enable(void)
{
    __asm__ volatile("sti" : : : "memory");
}

static inline void irq_disable(void)
{
    __asm__ volatile("cli" : : : "memory");
}

static inline void irq_restore_raw(uint64_t flags)
{
    if (flags & RFLAGS_IF)
        irq_enable();
}

void sched_preempt_point(void); /* sched.c: reschedule now if one is pending */

static inline void irq_restore(uint64_t flags)
{
    if (flags & RFLAGS_IF) {
        irq_enable();
        sched_preempt_point();
    }
}

/*
 * Spinlocks. The lock is a real atomic test-and-set so the code is SMP-ready, but Maleos
 * runs on one CPU for now: the irqsave variants are what actually provide mutual exclusion,
 * and a spin that never ends means self-deadlock, which is reported instead of hanging.
 */
typedef struct {
    volatile uint32_t locked;
} spinlock_t;

#define SPINLOCK_INIT                                                                              \
    {                                                                                              \
        0                                                                                          \
    }
#define SPIN_DEADLOCK_LIMIT 200000000UL

__attribute__((noreturn)) void spin_deadlock(spinlock_t *lock);

static inline void spin_lock(spinlock_t *l)
{
    unsigned long spins = 0;
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) {
            __builtin_ia32_pause();
            if (++spins > SPIN_DEADLOCK_LIMIT)
                spin_deadlock(l);
        }
    }
}

static inline void spin_unlock(spinlock_t *l)
{
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}

static inline uint64_t spin_lock_irqsave(spinlock_t *l)
{
    uint64_t flags = irq_save();
    spin_lock(l);
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags)
{
    spin_unlock(l);
    irq_restore(flags);
}

#endif

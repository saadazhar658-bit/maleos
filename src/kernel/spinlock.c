#include "kernel/spinlock.h"

#include "kernel/printk.h"

void spin_deadlock(spinlock_t *lock)
{
    kpanic("spinlock %p spun too long (self-deadlock?)", (void *)lock);
}

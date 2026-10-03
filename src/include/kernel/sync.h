#ifndef MALEOS_KERNEL_SYNC_H
#define MALEOS_KERNEL_SYNC_H

#include <stdbool.h>

#include "kernel/sched.h"
#include "kernel/spinlock.h"

/*
 * Sleeping locks. Use only from thread context (never from an interrupt handler).
 * No priority inheritance yet.
 */
struct mutex {
    spinlock_t lock;
    struct thread *owner;
    struct waitq wq;
};

void mutex_init(struct mutex *m);
void mutex_lock(struct mutex *m);
bool mutex_trylock(struct mutex *m);
void mutex_unlock(struct mutex *m);

struct semaphore {
    spinlock_t lock;
    int count;
    struct waitq wq;
};

void sem_init(struct semaphore *s, int initial);
void sem_wait(struct semaphore *s);
bool sem_trywait(struct semaphore *s);
void sem_post(struct semaphore *s);

#endif

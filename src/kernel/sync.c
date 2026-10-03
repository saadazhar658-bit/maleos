#include "kernel/sync.h"

#include "kernel/printk.h"

void mutex_init(struct mutex *m)
{
    m->lock = (spinlock_t)SPINLOCK_INIT;
    m->owner = NULL;
    waitq_init(&m->wq);
}

void mutex_lock(struct mutex *m)
{
    uint64_t flags = spin_lock_irqsave(&m->lock);
    KASSERT(m->owner != thread_current()); /* not recursive */
    while (m->owner)
        waitq_wait(&m->wq, &m->lock, 0);
    m->owner = thread_current();
    spin_unlock_irqrestore(&m->lock, flags);
}

bool mutex_trylock(struct mutex *m)
{
    uint64_t flags = spin_lock_irqsave(&m->lock);
    bool ok = (m->owner == NULL);
    if (ok)
        m->owner = thread_current();
    spin_unlock_irqrestore(&m->lock, flags);
    return ok;
}

void mutex_unlock(struct mutex *m)
{
    uint64_t flags = spin_lock_irqsave(&m->lock);
    KASSERT(m->owner == thread_current());
    m->owner = NULL;
    waitq_wake_one(&m->wq);
    spin_unlock_irqrestore(&m->lock, flags);
}

void sem_init(struct semaphore *s, int initial)
{
    s->lock = (spinlock_t)SPINLOCK_INIT;
    s->count = initial;
    waitq_init(&s->wq);
}

void sem_wait(struct semaphore *s)
{
    uint64_t flags = spin_lock_irqsave(&s->lock);
    while (s->count == 0)
        waitq_wait(&s->wq, &s->lock, 0);
    s->count--;
    spin_unlock_irqrestore(&s->lock, flags);
}

bool sem_trywait(struct semaphore *s)
{
    uint64_t flags = spin_lock_irqsave(&s->lock);
    bool ok = s->count > 0;
    if (ok)
        s->count--;
    spin_unlock_irqrestore(&s->lock, flags);
    return ok;
}

void sem_post(struct semaphore *s)
{
    uint64_t flags = spin_lock_irqsave(&s->lock);
    s->count++;
    waitq_wake_one(&s->wq);
    spin_unlock_irqrestore(&s->lock, flags);
}

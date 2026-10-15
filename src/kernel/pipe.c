#include "kernel/pipe.h"

#include "kernel/errno.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/spinlock.h"
#include "mm/heap.h"

struct pipe {
    bool used;
    uint8_t *buf; /* PIPE_BUF_SIZE bytes, allocated on creation */
    uint32_t head, count;
    int readers, writers; /* descriptors referring to each end */
    struct waitq wq;      /* readers wait for data, writers for space */
};

static struct pipe pipes[PIPE_MAX];
static spinlock_t pipe_lock = SPINLOCK_INIT;
static bool inited;

static struct pipe *get(int id)
{
    if (id < 0 || id >= PIPE_MAX || !pipes[id].used)
        kpanic("pipe: bad id %d", id);
    return &pipes[id];
}

int pipe_create(void)
{
    uint8_t *buf = kmalloc(PIPE_BUF_SIZE);
    if (!buf)
        return -ENOMEM;

    uint64_t f = spin_lock_irqsave(&pipe_lock);
    if (!inited) {
        for (int i = 0; i < PIPE_MAX; i++)
            waitq_init(&pipes[i].wq);
        inited = true;
    }
    for (int i = 0; i < PIPE_MAX; i++) {
        struct pipe *p = &pipes[i];
        if (p->used)
            continue;
        p->used = true;
        p->buf = buf;
        p->head = p->count = 0;
        p->readers = p->writers = 1;
        spin_unlock_irqrestore(&pipe_lock, f);
        return i;
    }
    spin_unlock_irqrestore(&pipe_lock, f);
    kfree(buf);
    return -ENFILE;
}

void pipe_ref(int id, int end)
{
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    struct pipe *p = get(id);
    if (end == PIPE_END_READ)
        p->readers++;
    else
        p->writers++;
    spin_unlock_irqrestore(&pipe_lock, f);
}

void pipe_close(int id, int end)
{
    uint8_t *to_free = NULL;
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    struct pipe *p = get(id);
    if (end == PIPE_END_READ)
        p->readers--;
    else
        p->writers--;
    waitq_wake_all(&p->wq); /* EOF for readers, EPIPE for writers */
    if (p->readers == 0 && p->writers == 0) {
        to_free = p->buf;
        p->buf = NULL;
        p->used = false;
    }
    spin_unlock_irqrestore(&pipe_lock, f);
    kfree(to_free);
}

int64_t pipe_read(int id, void *buf, uint64_t len)
{
    if (len == 0)
        return 0;
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    struct pipe *p = get(id);
    while (p->count == 0) {
        if (p->writers == 0) {
            spin_unlock_irqrestore(&pipe_lock, f);
            return 0;
        }
        waitq_wait(&p->wq, &pipe_lock, 0);
    }
    uint64_t n = len < p->count ? len : p->count;
    uint8_t *out = buf;
    for (uint64_t i = 0; i < n; i++)
        out[i] = p->buf[(p->head + i) % PIPE_BUF_SIZE];
    p->head = (uint32_t)((p->head + n) % PIPE_BUF_SIZE);
    p->count -= (uint32_t)n;
    waitq_wake_all(&p->wq); /* room for writers */
    spin_unlock_irqrestore(&pipe_lock, f);
    return (int64_t)n;
}

int64_t pipe_write(int id, const void *buf, uint64_t len)
{
    const uint8_t *in = buf;
    uint64_t done = 0;
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    struct pipe *p = get(id);
    while (done < len) {
        if (p->readers == 0) {
            spin_unlock_irqrestore(&pipe_lock, f);
            return done ? (int64_t)done : -EPIPE;
        }
        uint64_t room = PIPE_BUF_SIZE - p->count;
        if (room == 0) {
            waitq_wait(&p->wq, &pipe_lock, 0);
            continue;
        }
        uint64_t n = len - done < room ? len - done : room;
        for (uint64_t i = 0; i < n; i++)
            p->buf[(p->head + p->count + i) % PIPE_BUF_SIZE] = in[done + i];
        p->count += (uint32_t)n;
        done += n;
        waitq_wake_all(&p->wq); /* data for readers */
    }
    spin_unlock_irqrestore(&pipe_lock, f);
    return (int64_t)done;
}

int pipe_count(void)
{
    int n = 0;
    uint64_t f = spin_lock_irqsave(&pipe_lock);
    for (int i = 0; i < PIPE_MAX; i++)
        if (pipes[i].used)
            n++;
    spin_unlock_irqrestore(&pipe_lock, f);
    return n;
}

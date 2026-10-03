#include "kernel/ipc.h"

#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/spinlock.h"
#include "kernel/string.h"
#include "mm/heap.h"

/*
 * IPC ports: bounded ring buffers of fixed-size messages with blocking send/receive,
 * timeouts, and a request/reply helper built on a per-thread reply port.
 */

struct ipc_port {
    spinlock_t lock;
    char name[16];
    struct ipc_msg *buf;
    size_t cap, head, count;
    struct waitq senders;   /* blocked because the queue is full */
    struct waitq receivers; /* blocked because the queue is empty */
    unsigned waiters;       /* threads currently blocked inside send/recv */
    bool closed;
};

struct ipc_port *ipc_port_create(const char *name, size_t capacity)
{
    if (capacity == 0 || capacity > 1024)
        return NULL;

    struct ipc_port *p = kzalloc(sizeof(*p));
    if (!p)
        return NULL;
    p->buf = kzalloc(capacity * sizeof(struct ipc_msg));
    if (!p->buf) {
        kfree(p);
        return NULL;
    }

    p->cap = capacity;
    p->lock = (spinlock_t)SPINLOCK_INIT;
    waitq_init(&p->senders);
    waitq_init(&p->receivers);

    size_t n = strlen(name);
    if (n >= sizeof(p->name))
        n = sizeof(p->name) - 1;
    memcpy(p->name, name, n);
    return p;
}

static void port_free(struct ipc_port *p)
{
    kfree(p->buf);
    kfree(p);
}

void ipc_port_destroy(struct ipc_port *p)
{
    uint64_t flags = spin_lock_irqsave(&p->lock);
    p->closed = true;
    waitq_wake_all(&p->senders);
    waitq_wake_all(&p->receivers);
    bool free_now = (p->waiters == 0); /* otherwise the last thread to leave frees it */
    spin_unlock_irqrestore(&p->lock, flags);
    if (free_now)
        port_free(p);
}

/* Ticks left until `deadline`, or 0 if it has passed. */
static uint64_t remaining_ticks(uint64_t deadline)
{
    uint64_t now = timer_ticks();
    return now >= deadline ? 0 : deadline - now;
}

static uint64_t make_deadline(uint64_t timeout_ms)
{
    if (timeout_ms == IPC_FOREVER || timeout_ms == 0)
        return 0;
    return timer_ticks() + ms_to_ticks(timeout_ms) + 1;
}

/* Shared exit path: if the port was destroyed while we were blocked, the last one out frees it. */
static int leave(struct ipc_port *p, uint64_t flags, int rc)
{
    bool free_now = p->closed && p->waiters == 0;
    spin_unlock_irqrestore(&p->lock, flags);
    if (free_now)
        port_free(p);
    return rc;
}

int ipc_send_timeout(struct ipc_port *p, const struct ipc_msg *m, uint64_t timeout_ms)
{
    if (!p || !m || m->len > IPC_MAX_PAYLOAD)
        return IPC_EINVAL;

    bool forever = timeout_ms == IPC_FOREVER;
    uint64_t deadline = make_deadline(timeout_ms);

    uint64_t flags = spin_lock_irqsave(&p->lock);
    for (;;) {
        if (p->closed)
            return leave(p, flags, IPC_ECLOSED);
        if (p->count < p->cap) {
            struct ipc_msg *slot = &p->buf[(p->head + p->count) % p->cap];
            *slot = *m;
            slot->sender = thread_current()->tid;
            p->count++;
            waitq_wake_one(&p->receivers);
            return leave(p, flags, IPC_OK);
        }
        if (timeout_ms == 0)
            return leave(p, flags, IPC_EFULL);

        uint64_t wait = 0;
        if (!forever) {
            wait = remaining_ticks(deadline);
            if (wait == 0)
                return leave(p, flags, IPC_ETIMEOUT);
        }
        p->waiters++;
        waitq_wait(&p->senders, &p->lock, wait);
        p->waiters--;
    }
}

int ipc_recv_timeout(struct ipc_port *p, struct ipc_msg *m, uint64_t timeout_ms)
{
    if (!p || !m)
        return IPC_EINVAL;

    bool forever = timeout_ms == IPC_FOREVER;
    uint64_t deadline = make_deadline(timeout_ms);

    uint64_t flags = spin_lock_irqsave(&p->lock);
    for (;;) {
        if (p->closed)
            return leave(p, flags, IPC_ECLOSED);
        if (p->count > 0) {
            *m = p->buf[p->head];
            p->head = (p->head + 1) % p->cap;
            p->count--;
            waitq_wake_one(&p->senders);
            return leave(p, flags, IPC_OK);
        }
        if (timeout_ms == 0)
            return leave(p, flags, IPC_EEMPTY);

        uint64_t wait = 0;
        if (!forever) {
            wait = remaining_ticks(deadline);
            if (wait == 0)
                return leave(p, flags, IPC_ETIMEOUT);
        }
        p->waiters++;
        waitq_wait(&p->receivers, &p->lock, wait);
        p->waiters--;
    }
}

int ipc_call(struct ipc_port *server, const struct ipc_msg *req, struct ipc_msg *reply,
             uint64_t timeout_ms)
{
    struct thread *t = thread_current();
    if (!t->reply_port) {
        t->reply_port = ipc_port_create("reply", 2);
        if (!t->reply_port)
            return IPC_ENOMEM;
    }

    /* A reply to an earlier, timed-out call must not be mistaken for this one's. */
    struct ipc_msg stale;
    while (ipc_try_recv(t->reply_port, &stale) == IPC_OK)
        ;

    int rc = ipc_send_timeout(server, req, timeout_ms);
    if (rc != IPC_OK)
        return rc;
    return ipc_recv_timeout(t->reply_port, reply, timeout_ms);
}

int ipc_reply(const struct ipc_msg *req, const struct ipc_msg *reply)
{
    /* Look the caller up under the thread-table lock so a thread that has since exited
     * (taking its reply port with it) can never be dereferenced. */
    uint64_t flags = thread_table_lock();
    struct thread *caller = thread_lookup(req->sender);
    int rc = IPC_ECLOSED;
    if (caller && caller->reply_port)
        rc = ipc_try_send(caller->reply_port, reply);
    thread_table_unlock(flags);
    return rc;
}

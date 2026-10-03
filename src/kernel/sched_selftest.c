#include "kernel/sched_selftest.h"

#include "kernel/ipc.h"
#include "kernel/kstack.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/spinlock.h"
#include "kernel/string.h"
#include "kernel/sync.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

static int checks;

#define CHECK(cond, what)                                                                          \
    do {                                                                                           \
        checks++;                                                                                  \
        if (!(cond))                                                                               \
            kpanic("sched selftest failed: %s (%s:%d)", what, __FILE__, __LINE__);                 \
    } while (0)

/* ---------- spinlocks and interrupt state ---------- */

static void test_spinlock(void)
{
    spinlock_t l = SPINLOCK_INIT;
    CHECK(irq_enabled(), "interrupts are on during tests");

    uint64_t f = spin_lock_irqsave(&l);
    CHECK(!irq_enabled(), "irqsave disables interrupts");
    CHECK(l.locked == 1, "lock is held");
    spin_unlock_irqrestore(&l, f);
    CHECK(irq_enabled(), "irqrestore re-enables interrupts");
    CHECK(l.locked == 0, "lock is released");

    /* Nested: the inner restore must leave interrupts off. */
    spinlock_t l2 = SPINLOCK_INIT;
    uint64_t f1 = spin_lock_irqsave(&l);
    uint64_t f2 = spin_lock_irqsave(&l2);
    spin_unlock_irqrestore(&l2, f2);
    CHECK(!irq_enabled(), "inner unlock keeps interrupts off");
    spin_unlock_irqrestore(&l, f1);
    CHECK(irq_enabled(), "outer unlock restores interrupts");
}

/* ---------- timer ---------- */

static void test_timer(void)
{
    uint64_t t0 = timer_ticks();
    thread_sleep_ms(100);
    uint64_t dt = timer_ticks() - t0;
    CHECK(dt >= 10, "sleep(100ms) lasts at least 10 ticks");
    CHECK(dt <= 20, "sleep(100ms) does not overshoot wildly");

    t0 = timer_ticks();
    thread_sleep_ms(0); /* degenerates to a yield */
    CHECK(timer_ticks() - t0 <= 2, "sleep(0) returns promptly");
}

/* ---------- thread lifecycle ---------- */

static void exit_worker(void *arg)
{
    thread_exit((int)(uintptr_t)arg * 2);
}

static volatile int detached_ran;

static void detached_worker(void *arg)
{
    (void)arg;
    detached_ran = 1;
}

static void test_lifecycle(void)
{
    /* Warm-up: first use allocates page tables that stay allocated afterwards. */
    thread_join(thread_create("warm", exit_worker, (void *)1, SCHED_PRIO_NORMAL));

    uint64_t frames = pmm_free_frame_count();
    uint32_t threads = sched_thread_count();
    struct heap_stats h0, h1;
    heap_get_stats(&h0);

    struct thread *t[20];
    for (int i = 0; i < 20; i++) {
        t[i] = thread_create("exit", exit_worker, (void *)(uintptr_t)i, SCHED_PRIO_NORMAL);
        CHECK(t[i] != NULL, "thread_create");
    }
    int ok = 1;
    for (int i = 0; i < 20; i++)
        ok &= (thread_join(t[i]) == i * 2);
    CHECK(ok, "join returns each thread's exit code");

    heap_get_stats(&h1);
    CHECK(pmm_free_frame_count() == frames, "joined threads return all their frames");
    CHECK(sched_thread_count() == threads, "thread count restored");
    CHECK(h1.used_bytes == h0.used_bytes, "joined threads leave no heap leak");

    /* Detached threads are reaped by the idle thread. */
    detached_ran = 0;
    struct thread *d = thread_create("detached", detached_worker, NULL, SCHED_PRIO_NORMAL);
    thread_detach(d);
    thread_sleep_ms(60);
    CHECK(detached_ran == 1, "detached thread ran");
    CHECK(sched_thread_count() == threads, "detached thread was reaped");
    CHECK(pmm_free_frame_count() == frames, "reaped thread returned its frames");

    /* Kernel stack layout: unmapped guard page, then mapped stack pages. The new thread
     * cannot have run yet (we outrank it), so its stack is exactly as allocated. */
    struct thread *p = thread_create("probe", exit_worker, NULL, SCHED_PRIO_NORMAL);
    uint64_t pml4 = vmm_kernel_pml4();
    CHECK(!vmm_translate(pml4, p->kstack_base, NULL, NULL), "thread stack guard page unmapped");
    CHECK(vmm_translate(pml4, p->kstack_base + PAGE_SIZE, NULL, NULL), "thread stack mapped");
    CHECK(vmm_translate(pml4, p->kstack_base + KSTACK_PAGES * PAGE_SIZE, NULL, NULL),
          "top stack page mapped");
    CHECK(!vmm_translate(pml4, p->kstack_base + (KSTACK_PAGES + 1) * PAGE_SIZE, NULL, NULL),
          "page above the stack unmapped");
    thread_join(p);

    CHECK(thread_set_priority(thread_current(), 99) == -1, "bad priority rejected");
}

/* ---------- preemption and priorities ---------- */

static volatile bool stop_flag;
static volatile uint64_t counters[2];

static void spinner(void *arg)
{
    int i = (int)(uintptr_t)arg;
    while (!stop_flag)
        counters[i]++;
}

static void test_round_robin(void)
{
    stop_flag = false;
    counters[0] = counters[1] = 0;
    uint64_t sw0 = sched_context_switches();

    /* Two CPU hogs that never yield: only the timer can interleave them. */
    struct thread *a = thread_create("spin-a", spinner, (void *)0, SCHED_PRIO_NORMAL);
    struct thread *b = thread_create("spin-b", spinner, (void *)1, SCHED_PRIO_NORMAL);

    thread_sleep_ms(300); /* main runs at higher priority, so it wakes on time */
    stop_flag = true;
    thread_join(a);
    thread_join(b);

    uint64_t ca = counters[0], cb = counters[1];
    CHECK(ca > 0 && cb > 0, "both equal-priority hogs made progress");
    CHECK(sched_context_switches() - sw0 >= 6, "timer preempted the hogs repeatedly");
    uint64_t hi = ca > cb ? ca : cb, lo = ca > cb ? cb : ca;
    CHECK(lo * 4 >= hi, "equal-priority threads share the CPU fairly");
}

static volatile uint64_t order_seq;
static volatile uint64_t low_started_at, high_done_at;

static void prio_low(void *arg)
{
    (void)arg;
    low_started_at = __atomic_add_fetch(&order_seq, 1, __ATOMIC_SEQ_CST);
}

static void prio_high(void *arg)
{
    (void)arg;
    volatile uint64_t x = 0;
    for (uint64_t i = 0; i < 3000000; i++) /* long enough to span several timer ticks */
        x += i;
    high_done_at = __atomic_add_fetch(&order_seq, 1, __ATOMIC_SEQ_CST);
}

static volatile uint64_t hi_wake_tick;
static volatile int hi_ran;

static void prio_waker(void *arg)
{
    (void)arg;
    thread_sleep_ms(30);
    hi_wake_tick = timer_ticks();
    hi_ran = 1;
}

static void test_priorities(void)
{
    /* Strict priority: the low thread may only start once the high one is finished. */
    order_seq = 0;
    low_started_at = high_done_at = 0;
    struct thread *lo = thread_create("prio-low", prio_low, NULL, 6);
    struct thread *hi = thread_create("prio-high", prio_high, NULL, 3);
    thread_join(hi);
    thread_join(lo);
    CHECK(high_done_at != 0 && low_started_at != 0, "both priority threads ran");
    CHECK(low_started_at > high_done_at, "lower priority waited for the higher one");

    /* A sleeper that wakes must preempt a CPU hog without waiting for the hog's slice. */
    stop_flag = false;
    counters[0] = counters[1] = 0;
    hi_ran = 0;
    struct thread *hog = thread_create("hog", spinner, (void *)0, 6);
    thread_sleep_ms(20); /* let the hog get the CPU */
    uint64_t start = timer_ticks();
    struct thread *w = thread_create("waker", prio_waker, NULL, 2);
    thread_sleep_ms(80);
    CHECK(hi_ran == 1, "high-priority sleeper woke up while a hog was running");
    CHECK(hi_wake_tick - start <= 8, "wakeup preempted the hog within a few ticks");
    stop_flag = true;
    thread_join(hog);
    thread_join(w);

    /* Changing priority takes effect. */
    int old = thread_current()->prio;
    CHECK(thread_set_priority(thread_current(), 1) == 0, "set priority");
    CHECK(thread_current()->prio == 1, "priority changed");
    thread_set_priority(thread_current(), old);
}

/* ---------- mutex and semaphore ---------- */

static struct mutex test_mutex;
static volatile int shared_counter;
static volatile int in_critical;
static volatile int violations;

static void mutex_worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < 50; i++) {
        mutex_lock(&test_mutex);
        if (++in_critical != 1)
            violations++;
        int tmp = shared_counter;
        if (i % 25 == 0)
            thread_sleep_ms(1); /* hold the lock across a sleep: others must block */
        else
            sched_yield();
        shared_counter = tmp + 1; /* lost updates would show up here */
        in_critical--;
        mutex_unlock(&test_mutex);
    }
}

static void test_mutex_fn(void)
{
    mutex_init(&test_mutex);
    shared_counter = in_critical = violations = 0;

    CHECK(mutex_trylock(&test_mutex), "trylock on a free mutex");
    CHECK(!mutex_trylock(&test_mutex), "trylock on a held mutex fails");
    mutex_unlock(&test_mutex);

    struct thread *t[4];
    for (int i = 0; i < 4; i++)
        t[i] = thread_create("mutex", mutex_worker, NULL, SCHED_PRIO_NORMAL);
    for (int i = 0; i < 4; i++)
        thread_join(t[i]);

    CHECK(shared_counter == 200, "no lost updates under the mutex");
    CHECK(violations == 0, "mutual exclusion held");
    CHECK(test_mutex.owner == NULL, "mutex released");
}

static struct semaphore sem_items, sem_slots;
static volatile int ring[4];
static volatile int ring_head, ring_tail;
static volatile int consumed_sum;

static void sem_producer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= 40; i++) {
        sem_wait(&sem_slots);
        ring[ring_head++ % 4] = i;
        sem_post(&sem_items);
    }
}

static void sem_consumer(void *arg)
{
    (void)arg;
    for (int i = 1; i <= 40; i++) {
        sem_wait(&sem_items);
        int v = ring[ring_tail++ % 4];
        sem_post(&sem_slots);
        if (v != i)
            consumed_sum = -1000000;
        else
            consumed_sum += v;
    }
}

static void test_semaphore(void)
{
    sem_init(&sem_items, 0);
    sem_init(&sem_slots, 4);
    ring_head = ring_tail = consumed_sum = 0;

    CHECK(!sem_trywait(&sem_items), "trywait on an empty semaphore fails");
    sem_post(&sem_items);
    CHECK(sem_trywait(&sem_items), "trywait after post succeeds");

    struct thread *c = thread_create("consumer", sem_consumer, NULL, SCHED_PRIO_NORMAL);
    struct thread *p = thread_create("producer", sem_producer, NULL, SCHED_PRIO_NORMAL);
    thread_join(p);
    thread_join(c);
    CHECK(consumed_sum == 40 * 41 / 2, "bounded producer/consumer delivers everything in order");
}

/* ---------- IPC ---------- */

static struct ipc_port *pipe_port;

static void ipc_producer(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < 100; i++) {
        struct ipc_msg m = {.type = i, .len = 4};
        memcpy(m.payload, &i, 4);
        if (ipc_send(pipe_port, &m) != IPC_OK)
            thread_exit(-1);
    }
}

static void test_ipc_pipeline(void)
{
    pipe_port = ipc_port_create("pipe", 4);
    CHECK(pipe_port != NULL, "port create");

    struct ipc_msg m;
    CHECK(ipc_try_recv(pipe_port, &m) == IPC_EEMPTY, "try_recv on empty port");

    struct thread *p = thread_create("ipc-prod", ipc_producer, NULL, SCHED_PRIO_NORMAL);
    uint64_t producer_tid = p->tid;
    thread_sleep_ms(60); /* producer fills the 4 slots, then blocks on a full queue */

    int in_order = 1;
    int sender_ok = 1;
    for (uint32_t i = 0; i < 100; i++) {
        CHECK(ipc_recv(pipe_port, &m) == IPC_OK, "recv");
        uint32_t v;
        memcpy(&v, m.payload, 4);
        in_order &= (m.type == i && v == i && m.len == 4);
        sender_ok &= (m.sender == producer_tid);
    }
    CHECK(in_order, "100 messages arrive intact and in order");
    CHECK(sender_ok, "sender tid is stamped by the kernel");
    CHECK(thread_join(p) == 0, "producer finished cleanly");

    /* Non-blocking send on a full queue. */
    struct ipc_msg x = {.type = 7};
    int sent = 0;
    for (int i = 0; i < 4; i++)
        sent += (ipc_try_send(pipe_port, &x) == IPC_OK);
    CHECK(sent == 4, "queue accepts exactly its capacity");
    CHECK(ipc_try_send(pipe_port, &x) == IPC_EFULL, "try_send on a full port");
    CHECK(ipc_send_timeout(pipe_port, &x, 30) == IPC_ETIMEOUT, "send times out on a full port");

    struct ipc_msg big = {.len = IPC_MAX_PAYLOAD + 1};
    CHECK(ipc_try_send(pipe_port, &big) == IPC_EINVAL, "oversized payload rejected");

    ipc_port_destroy(pipe_port);
}

static void test_ipc_timeout(void)
{
    struct ipc_port *p = ipc_port_create("empty", 2);
    struct ipc_msg m;
    uint64_t t0 = timer_ticks();
    CHECK(ipc_recv_timeout(p, &m, 50) == IPC_ETIMEOUT, "recv times out on an empty port");
    uint64_t dt = timer_ticks() - t0;
    CHECK(dt >= 5 && dt <= 15, "timeout lasts about as long as requested");
    ipc_port_destroy(p);
}

static struct ipc_port *server_port;

static void echo_server(void *arg)
{
    (void)arg;
    struct ipc_msg req, rep;
    for (;;) {
        if (ipc_recv(server_port, &req) != IPC_OK)
            thread_exit(-1);
        if (req.type == 0xDEAD)
            thread_exit(0);
        rep = req;
        rep.type = req.type + 1;
        if (req.len >= 4) {
            uint32_t v;
            memcpy(&v, req.payload, 4);
            v *= 3;
            memcpy(rep.payload, &v, 4);
        }
        if (ipc_reply(&req, &rep) != IPC_OK)
            thread_exit(-2);
    }
}

static void test_ipc_call(void)
{
    server_port = ipc_port_create("echo", 4);
    struct thread *s = thread_create("echo-srv", echo_server, NULL, SCHED_PRIO_NORMAL);
    uint64_t server_tid = s->tid;

    int ok = 1;
    for (uint32_t i = 1; i <= 10; i++) {
        struct ipc_msg req = {.type = 100 + i, .len = 4}, rep;
        memcpy(req.payload, &i, 4);
        int rc = ipc_call(server_port, &req, &rep, 1000);
        uint32_t v = 0;
        memcpy(&v, rep.payload, 4);
        ok &= (rc == IPC_OK && rep.type == 101 + i && v == i * 3 && rep.sender == server_tid);
    }
    CHECK(ok, "10 request/reply round trips");

    struct ipc_msg quit = {.type = 0xDEAD};
    CHECK(ipc_send(server_port, &quit) == IPC_OK, "send shutdown request");
    CHECK(thread_join(s) == 0, "server exited cleanly");

    /* No server is listening any more: the call must time out, not hang. */
    struct ipc_msg req = {.type = 1}, rep;
    CHECK(ipc_call(server_port, &req, &rep, 40) == IPC_ETIMEOUT, "call times out without a reply");
    ipc_port_destroy(server_port);
}

static struct ipc_port *doomed_port;

static void doomed_receiver(void *arg)
{
    (void)arg;
    struct ipc_msg m;
    thread_exit(ipc_recv(doomed_port, &m));
}

static void test_ipc_destroy(void)
{
    doomed_port = ipc_port_create("doomed", 2);
    struct thread *r = thread_create("doomed", doomed_receiver, NULL, SCHED_PRIO_NORMAL);
    thread_sleep_ms(30); /* receiver is now blocked inside ipc_recv */
    ipc_port_destroy(doomed_port);
    CHECK(thread_join(r) == IPC_ECLOSED, "destroying a port wakes blocked receivers with ECLOSED");

    /* Replying to a thread that no longer exists must fail cleanly. */
    struct ipc_msg ghost = {.sender = 0xFFFFFF}, rep = {0};
    CHECK(ipc_reply(&ghost, &rep) == IPC_ECLOSED, "reply to an unknown thread fails cleanly");
}

/* ---------- allocators under preemption ---------- */

static volatile int heap_failures;

static void heap_worker(void *arg)
{
    uint32_t seed = 0x9E3779B9u + (uint32_t)(uintptr_t)arg * 977u;
    void *ptrs[16] = {0};
    size_t sizes[16] = {0};
    uint8_t pats[16] = {0};

    for (int i = 0; i < 300; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        int k = seed % 16;

        if (ptrs[k]) {
            uint8_t *p = ptrs[k];
            for (size_t j = 0; j < sizes[k]; j++) {
                if (p[j] != pats[k]) {
                    heap_failures++;
                    break;
                }
            }
            kfree(p);
            ptrs[k] = NULL;
        } else {
            size_t sz = 16 + (seed >> 8) % 700;
            uint8_t *p = kmalloc(sz);
            if (!p) {
                heap_failures++;
                continue;
            }
            pats[k] = (uint8_t)(seed >> 24) | 1;
            memset(p, pats[k], sz);
            ptrs[k] = p;
            sizes[k] = sz;
        }
        if (i % 7 == 0)
            sched_yield();
    }

    for (int k = 0; k < 16; k++)
        kfree(ptrs[k]);
}

static void test_heap_concurrency(void)
{
    struct heap_stats h0, h1;
    heap_get_stats(&h0);
    heap_failures = 0;

    struct thread *t[4];
    for (int i = 0; i < 4; i++)
        t[i] = thread_create("heap", heap_worker, (void *)(uintptr_t)i, SCHED_PRIO_NORMAL);
    for (int i = 0; i < 4; i++)
        thread_join(t[i]);

    heap_get_stats(&h1);
    CHECK(heap_failures == 0, "no corrupted or failed allocations across 4 preempted threads");
    CHECK(heap_check() == 0, "heap consistent after concurrent use");
    CHECK(h1.used_bytes == h0.used_bytes, "no leaked heap bytes");
}

int sched_selftest(void)
{
    checks = 0;
    int saved_prio = thread_current()->prio;
    thread_set_priority(thread_current(), 1); /* the test driver must always wake promptly */

    test_spinlock();
    test_timer();
    test_lifecycle();
    test_round_robin();
    test_priorities();
    test_mutex_fn();
    test_semaphore();
    test_ipc_pipeline();
    test_ipc_timeout();
    test_ipc_call();
    test_ipc_destroy();
    test_heap_concurrency();

    thread_set_priority(thread_current(), saved_prio);
    return checks;
}

#include "kernel/sched.h"

#include "arch/apic.h"
#include "arch/irq.h"
#include "kernel/ipc.h"
#include "kernel/kstack.h"
#include "kernel/printk.h"
#include "kernel/string.h"
#include "mm/heap.h"
#include "mm/mm.h"

/*
 * Scheduler: strict priority, round-robin within a priority, preemptive.
 *
 * Uniprocessor design: the run queues, sleep list and thread table are protected by
 * disabling interrupts. Every blocking primitive (waitq_wait) is entered with interrupts
 * off, so a wakeup cannot slip in between "decide to block" and "actually switch away".
 * schedule() is always called with interrupts off and returns, possibly much later, in
 * the same state.
 */

extern void context_switch(struct context **old, struct context *next);

volatile int irq_depth;
volatile bool need_resched;

static bool sched_running;
static struct thread *current_thread;

static struct list_node runq[SCHED_PRIO_LEVELS];
static uint32_t runq_bits; /* bit n set <=> runq[n] is not empty */
static struct list_node sleepers;
static struct list_node all_threads;
static struct list_node zombies; /* detached zombies, freed by the idle thread */

static volatile uint64_t ticks;
static uint64_t next_tid = 1;
static uint64_t switches;
static uint32_t thread_count;

/* ---------- run queue ---------- */

static void enqueue(struct thread *t)
{
    t->state = T_READY;
    list_add_tail(&runq[t->prio], &t->sched_node);
    runq_bits |= 1u << t->prio;
}

static void runq_remove(struct thread *t)
{
    list_del(&t->sched_node);
    if (list_empty(&runq[t->prio]))
        runq_bits &= ~(1u << t->prio);
}

static struct thread *dequeue_best(void)
{
    if (!runq_bits)
        return NULL;
    int p = __builtin_ctz(runq_bits);
    struct thread *t = container_of(runq[p].next, struct thread, sched_node);
    runq_remove(t);
    return t;
}

static void make_ready(struct thread *t)
{
    enqueue(t);
    if (current_thread && t->prio < current_thread->prio)
        need_resched = true;
}

/* ---------- switching ---------- */

static void schedule(void)
{
    KASSERT(!irq_enabled());
    KASSERT(irq_depth == 0);

    struct thread *prev = current_thread;
    struct thread *next = dequeue_best();
    if (!next)
        kpanic("scheduler: nothing to run");

    next->slice = SCHED_QUANTUM_TICKS;
    if (next == prev) {
        prev->state = T_RUNNING;
        return;
    }

    next->state = T_RUNNING;
    current_thread = next;
    switches++;
    context_switch(&prev->ctx, next->ctx);
    /* We are running again; `current_thread` is us. */
}

static void preempt_current(void)
{
    struct thread *t = current_thread;
    if (t->state == T_RUNNING)
        enqueue(t); /* back of its queue: round robin */
    schedule();
}

void sched_preempt_point(void)
{
    if (!need_resched || irq_depth || !sched_running)
        return;
    uint64_t f = irq_save();
    if (need_resched) {
        need_resched = false;
        preempt_current();
    }
    irq_restore_raw(f);
}

void sched_irq_exit(void)
{
    need_resched = false;
    if (sched_running)
        preempt_current();
}

void sched_yield(void)
{
    uint64_t f = irq_save();
    preempt_current();
    irq_restore_raw(f);
}

/* ---------- wait queues and sleeping ---------- */

void waitq_init(struct waitq *q)
{
    list_init(&q->list);
}

static void wake_from_timeout(struct thread *t)
{
    list_del(&t->sleep_node);
    t->sleeping = false;
    if (t->wq) { /* it was a timed wait on a queue, not a plain sleep */
        list_del(&t->sched_node);
        t->wq = NULL;
        t->timed_out = true;
    }
    make_ready(t);
}

int waitq_wait(struct waitq *q, spinlock_t *lock, uint64_t timeout_ticks)
{
    KASSERT(!irq_enabled());
    KASSERT(sched_running);

    struct thread *t = current_thread;
    t->state = T_BLOCKED;
    t->wq = q;
    t->timed_out = false;
    list_add_tail(&q->list, &t->sched_node);

    if (timeout_ticks) {
        t->wake_tick = ticks + timeout_ticks;
        t->sleeping = true;
        list_add_tail(&sleepers, &t->sleep_node);
    }

    if (lock)
        spin_unlock(lock);
    schedule();
    if (lock)
        spin_lock(lock);
    return t->timed_out ? -1 : 0;
}

struct thread *waitq_wake_one(struct waitq *q)
{
    KASSERT(!irq_enabled());
    if (list_empty(&q->list))
        return NULL;

    struct thread *t = container_of(q->list.next, struct thread, sched_node);
    list_del(&t->sched_node);
    t->wq = NULL;
    if (t->sleeping) {
        list_del(&t->sleep_node);
        t->sleeping = false;
    }
    make_ready(t);
    return t;
}

void waitq_wake_all(struct waitq *q)
{
    while (waitq_wake_one(q))
        ;
}

uint64_t timer_ticks(void)
{
    return ticks;
}

uint64_t ms_to_ticks(uint64_t ms)
{
    return (ms * TIMER_HZ + 999) / 1000;
}

void thread_sleep_ticks(uint64_t n)
{
    if (n == 0) {
        sched_yield();
        return;
    }

    uint64_t f = irq_save();
    struct thread *t = current_thread;
    t->state = T_SLEEPING;
    t->wq = NULL;
    t->wake_tick = ticks + n + 1; /* +1: the current tick is already partly gone */
    t->sleeping = true;
    list_add_tail(&sleepers, &t->sleep_node);
    schedule();
    irq_restore_raw(f);
}

void thread_sleep_ms(uint64_t ms)
{
    thread_sleep_ticks(ms_to_ticks(ms));
}

static void timer_irq(struct interrupt_frame *frame)
{
    (void)frame;
    ticks++;
    current_thread->cpu_ticks++;

    struct list_node *n = sleepers.next;
    while (n != &sleepers) {
        struct list_node *next = n->next;
        struct thread *t = container_of(n, struct thread, sleep_node);
        if (ticks >= t->wake_tick)
            wake_from_timeout(t);
        n = next;
    }

    if (current_thread->state == T_RUNNING && --current_thread->slice <= 0)
        need_resched = true;
}

/* ---------- threads ---------- */

struct thread *thread_current(void)
{
    return current_thread;
}

static void thread_trampoline(void)
{
    struct thread *t = current_thread;
    irq_enable(); /* we arrive here from schedule(), with interrupts off */
    t->entry(t->arg);
    thread_exit(0);
}

static void idle_entry(void *arg);

static struct thread *thread_alloc(const char *name, int prio)
{
    struct thread *t = kzalloc(sizeof(*t));
    if (!t)
        return NULL;

    size_t n = strlen(name);
    if (n >= THREAD_NAME_MAX)
        n = THREAD_NAME_MAX - 1;
    memcpy(t->name, name, n);
    t->prio = prio;
    t->kstack_slot = -1;
    list_init(&t->sched_node);
    list_init(&t->sleep_node);
    list_init(&t->all_node);
    waitq_init(&t->joiners);
    return t;
}

static void register_thread(struct thread *t)
{
    t->tid = next_tid++;
    list_add_tail(&all_threads, &t->all_node);
    thread_count++;
}

struct thread *thread_create(const char *name, void (*entry)(void *), void *arg, int prio)
{
    if (prio < 0 || prio >= SCHED_PRIO_LEVELS || !entry)
        return NULL;

    struct thread *t = thread_alloc(name, prio);
    if (!t)
        return NULL;

    uint64_t top = kstack_alloc(&t->kstack_slot, &t->kstack_base);
    if (!top) {
        kfree(t);
        return NULL;
    }

    t->entry = entry;
    t->arg = arg;

    /*
     * Build the initial stack so the first context_switch() "returns" into the trampoline:
     *   top-8   fake return address (keeps the ABI stack alignment)
     *   top-16  trampoline address, popped by `ret`
     *   below   six zeroed callee-saved registers
     */
    uint64_t *sp = (uint64_t *)top;
    *--sp = 0;
    *--sp = (uint64_t)thread_trampoline;
    for (int i = 0; i < 6; i++)
        *--sp = 0;
    t->ctx = (struct context *)sp;

    uint64_t f = irq_save();
    register_thread(t);
    make_ready(t);
    irq_restore(f); /* may preempt us right away if t has higher priority */
    return t;
}

static void free_thread(struct thread *t)
{
    if (t->reply_port)
        ipc_port_destroy(t->reply_port);
    kstack_free(t->kstack_slot);
    kfree(t);
}

void thread_exit(int code)
{
    irq_disable();
    struct thread *t = current_thread;
    t->exit_code = code;
    t->state = T_ZOMBIE;
    waitq_wake_all(&t->joiners);

    if (t->detached)
        list_add_tail(&zombies, &t->sched_node);

    schedule(); /* never returns: a zombie is never put back on a queue */
    kpanic("zombie thread %s was rescheduled", t->name);
}

int thread_join(struct thread *t)
{
    uint64_t f = irq_save();
    KASSERT(t != current_thread);
    KASSERT(!t->detached);

    while (t->state != T_ZOMBIE)
        waitq_wait(&t->joiners, NULL, 0);

    int code = t->exit_code;
    list_del(&t->all_node);
    thread_count--;
    irq_restore_raw(f);

    free_thread(t);
    return code;
}

void thread_detach(struct thread *t)
{
    uint64_t f = irq_save();
    t->detached = true;
    if (t->state == T_ZOMBIE)
        list_add_tail(&zombies, &t->sched_node);
    irq_restore_raw(f);
}

static void reap_zombies(void)
{
    for (;;) {
        uint64_t f = irq_save();
        if (list_empty(&zombies)) {
            irq_restore_raw(f);
            return;
        }
        struct thread *t = container_of(zombies.next, struct thread, sched_node);
        list_del(&t->sched_node);
        list_del(&t->all_node);
        thread_count--;
        irq_restore_raw(f);
        free_thread(t);
    }
}

int thread_set_priority(struct thread *t, int prio)
{
    if (prio < 0 || prio >= SCHED_PRIO_LEVELS)
        return -1;

    uint64_t f = irq_save();
    if (t->state == T_READY) {
        runq_remove(t);
        t->prio = prio;
        enqueue(t);
    } else {
        t->prio = prio;
    }
    need_resched = true; /* let the scheduler re-evaluate */
    irq_restore(f);
    return 0;
}

static void idle_entry(void *arg)
{
    (void)arg;
    for (;;) {
        reap_zombies();
        __asm__ volatile("sti; hlt" : : : "memory");
    }
}

/* ---------- setup ---------- */

void sched_init(void)
{
    for (int i = 0; i < SCHED_PRIO_LEVELS; i++)
        list_init(&runq[i]);
    list_init(&sleepers);
    list_init(&all_threads);
    list_init(&zombies);

    /* The code that is running right now becomes the "boot" thread. */
    struct thread *boot = thread_alloc("boot", SCHED_PRIO_NORMAL);
    if (!boot)
        kpanic("sched_init: out of memory");
    boot->state = T_RUNNING;
    boot->slice = SCHED_QUANTUM_TICKS;
    register_thread(boot);
    current_thread = boot;

    if (!thread_create("idle", idle_entry, NULL, SCHED_PRIO_IDLE))
        kpanic("sched_init: cannot create the idle thread");
    sched_running = true;
}

void sched_start(void)
{
    apic_init();
    irq_register(VEC_TIMER, timer_irq);
    apic_timer_start(TIMER_HZ);
    irq_enable();
}

/* ---------- introspection ---------- */

uint64_t sched_context_switches(void)
{
    return switches;
}

uint32_t sched_thread_count(void)
{
    return thread_count;
}

uint64_t thread_table_lock(void)
{
    return irq_save();
}

void thread_table_unlock(uint64_t flags)
{
    irq_restore(flags);
}

struct thread *thread_lookup(uint64_t tid)
{
    for (struct list_node *n = all_threads.next; n != &all_threads; n = n->next) {
        struct thread *t = container_of(n, struct thread, all_node);
        if (t->tid == tid)
            return t;
    }
    return NULL;
}

void sched_dump(void)
{
    static const char *const names[] = {"ready", "running", "blocked", "sleeping", "zombie"};
    uint64_t f = irq_save();
    printk("tid  prio  state     cpu-ticks  name\n");
    for (struct list_node *n = all_threads.next; n != &all_threads; n = n->next) {
        struct thread *t = container_of(n, struct thread, all_node);
        printk("%3lu  %4d  %-8s  %9lu  %s\n", (unsigned long)t->tid, t->prio, names[t->state],
               (unsigned long)t->cpu_ticks, t->name);
    }
    irq_restore_raw(f);
}

#include "kernel/sched.h"

#include "arch/apic.h"
#include "arch/irq.h"
#include "arch/percpu.h"
#include "kernel/ipc.h"
#include "kernel/kstack.h"
#include "kernel/printk.h"
#include "kernel/smp.h"
#include "kernel/string.h"
#include "mm/heap.h"
#include "mm/mm.h"

/*
 * Scheduler: strict priority, round-robin within a priority, preemptive.
 *
 * One global run queue shared by all CPUs. Kernel code runs under the big kernel lock (see
 * arch/percpu.h), so the run queues, sleep list and thread table need no lock of their
 * own beyond disabling interrupts on the CPU that holds it. Every blocking primitive
 * (waitq_wait) is entered with interrupts off, so a wakeup cannot slip in between "decide
 * to block" and "actually switch away". schedule() is always called with interrupts off
 * and returns, possibly much later (and on another CPU), in the same state.
 *
 * Every CPU has its own idle thread. It is never on a run queue: a CPU falls back to it
 * when nothing else is ready, and only that CPU ever runs it.
 */

extern void context_switch(struct context **old, struct context *next);

static bool sched_running;
static volatile int cpu_limit = 1; /* CPUs allowed to run threads (the rest just idle) */

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

/*
 * A thread became ready: make some CPU notice. Prefer an idle CPU that has no reschedule
 * pending, otherwise the CPU running the lowest-priority thread, if that is worse than `t`.
 */
static void kick_for(struct thread *t)
{
    struct cpu *me = this_cpu();
    struct cpu *target = NULL;
    int worst = -1;

    /* The CPU running this code already holds the kernel lock, so an idle `me` is the cheapest
     * place for the thread: no IPI, no lock hand-over. Otherwise start at a pseudo-random CPU
     * so idle CPUs share the work. */
    if (me->online && (int)me->id < cpu_limit && me->current && me->current->is_idle &&
        !me->resched) {
        me->resched = true;
        return;
    }
    unsigned start = (unsigned)(__builtin_ia32_rdtsc() >> 5);
    for (int k = 0; k < cpu_limit; k++) {
        struct cpu *c = &cpus[(start + (unsigned)k) % (unsigned)cpu_limit];
        struct thread *cur = c->current;
        if (!c->online || !cur)
            continue;
        if (cur->is_idle) {
            if (!c->resched) {
                target = c;
                break;
            }
            continue;
        }
        if (cur->prio > t->prio && cur->prio > worst) {
            worst = cur->prio;
            target = c;
        }
    }
    if (!target)
        return;
    target->resched = true;
    if (target != me)
        apic_send_ipi(target->apic_id, VEC_RESCHED);
}

static void make_ready(struct thread *t)
{
    enqueue(t);
    kick_for(t);
}

/* ---------- switching ---------- */

static void schedule(void)
{
    KASSERT(!irq_enabled());

    struct cpu *c = this_cpu();
    KASSERT(c->irq_nest == 0);

    struct thread *prev = c->current;
    struct thread *next = NULL;
    if ((int)c->id < cpu_limit)
        next = dequeue_best();
    if (!next)
        next = c->idle;

    next->slice = SCHED_QUANTUM_TICKS;
    if (next == prev) {
        prev->state = T_RUNNING;
        return;
    }

    next->state = T_RUNNING;
    c->current = next;
    if (!next->is_idle)
        c->picks++;
    switches++;
    sched_switch_hook(next);
    context_switch(&prev->ctx, next->ctx);
    /* We are running again; `current_thread` is us. */
}

static void preempt_current(void)
{
    struct thread *t = thread_current();
    if (t->state == T_RUNNING && !t->is_idle)
        enqueue(t); /* back of its queue: round robin */
    schedule();
}

void sched_preempt_point(void)
{
    struct cpu *c = this_cpu();
    if (!c->resched || c->irq_nest || !sched_running)
        return;
    uint64_t f = irq_save();
    c = this_cpu();
    if (c->resched) {
        c->resched = false;
        preempt_current();
    }
    irq_restore_raw(f);
}

void sched_irq_exit(void)
{
    this_cpu()->resched = false;
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

    struct thread *t = thread_current();
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
    struct thread *t = thread_current();
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
    struct cpu *c = this_cpu();
    struct thread *cur = c->current;
    c->ticks++;
    cur->cpu_ticks++;
    if (!cur->is_idle)
        c->stall = 0;
    if (cur->is_idle)
        c->idle_ticks++;
    else if ((frame->cs & 3) == 3)
        c->user_ticks++;

    if (c->id == 0) { /* the boot CPU keeps the clock and the sleep list */
        ticks++;
        struct list_node *n = sleepers.next;
        while (n != &sleepers) {
            struct list_node *next = n->next;
            struct thread *t = container_of(n, struct thread, sleep_node);
            if (ticks >= t->wake_tick)
                wake_from_timeout(t);
            n = next;
        }
    }

    if (cur->is_idle) {
        /* Safety net for a lost wake-up: work has been waiting for two whole ticks. The first
         * tick is left alone because the CPU that readied the thread has kicked someone. */
        if (runq_bits && (int)c->id < cpu_limit) {
            if (++c->stall >= 2)
                c->resched = true;
        } else {
            c->stall = 0;
        }
    } else if (cur->state == T_RUNNING && --cur->slice <= 0) {
        c->resched = true;
    }
}

/* ---------- threads ---------- */

struct thread *thread_current(void)
{
    return this_cpu()->current;
}

static void thread_trampoline(void)
{
    struct thread *t = thread_current();
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

static struct thread *thread_create_common(const char *name, void (*entry)(void *), void *arg,
                                           int prio, bool idle)
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
    t->kstack_top = top;

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
    if (idle) {
        t->is_idle = true;
        t->state = T_READY; /* runs only when its own CPU picks it */
    } else {
        make_ready(t);
    }
    irq_restore(f); /* may preempt us right away if t has higher priority */
    return t;
}

struct thread *thread_create(const char *name, void (*entry)(void *), void *arg, int prio)
{
    return thread_create_common(name, entry, arg, prio, false);
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
    struct thread *t = thread_current();
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
    KASSERT(t != thread_current());
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
    this_cpu()->resched = true; /* let the scheduler re-evaluate */
    irq_restore(f);
    return 0;
}

static void idle_entry(void *arg)
{
    (void)arg;
    for (;;) {
        reap_zombies();
        cpu_idle_wait(); /* drops the big kernel lock while halted */
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
    this_cpu()->current = boot;

    struct thread *idle = thread_create_common("idle", idle_entry, NULL, SCHED_PRIO_IDLE, true);
    if (!idle)
        kpanic("sched_init: cannot create the idle thread");
    this_cpu()->idle = idle;
    this_cpu()->online = true;
    sched_running = true;
}

/* ---------- other CPUs ---------- */

/* Boot CPU, before the AP starts: give it an idle thread that adopts the AP's start-up stack. */
void sched_prepare_ap(struct cpu *c, uint64_t stack_top)
{
    char name[THREAD_NAME_MAX] = "idle/";
    name[5] = (char)('0' + c->id);
    name[6] = 0;
    struct thread *t = thread_alloc(name, SCHED_PRIO_IDLE);
    if (!t)
        kpanic("sched_prepare_ap: out of memory");
    t->is_idle = true;
    t->state = T_RUNNING;
    t->kstack_top = stack_top;
    uint64_t f = irq_save();
    register_thread(t);
    irq_restore_raw(f);
    c->idle = t;
}

/* Runs on the new CPU, under the big kernel lock: become its idle thread and start ticking. */
__attribute__((noreturn)) void sched_ap_enter(struct cpu *c)
{
    c->current = c->idle;
    c->online = true;
    cpu_count++;
    apic_timer_start_ap(TIMER_HZ);
    printk("SMP: CPU %u online (APIC id %u)\n", c->id, c->apic_id);
    idle_entry(NULL);
    __builtin_unreachable();
}

void sched_set_cpu_limit(int n)
{
    if (n < 1)
        n = 1;
    if (n > SMP_MAX)
        n = SMP_MAX;
    uint64_t f = irq_save();
    cpu_limit = n;
    irq_restore_raw(f);
}

int sched_cpu_limit(void)
{
    return cpu_limit;
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

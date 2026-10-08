#ifndef MALEOS_KERNEL_SCHED_H
#define MALEOS_KERNEL_SCHED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kernel/list.h"
#include "kernel/spinlock.h"

/* Priorities: 0 is the highest, SCHED_PRIO_IDLE the lowest (reserved for the idle thread). */
#define SCHED_PRIO_LEVELS 8
#define SCHED_PRIO_HIGHEST 0
#define SCHED_PRIO_NORMAL 4
#define SCHED_PRIO_IDLE 7

#define TIMER_HZ 100          /* scheduler tick: 10 ms */
#define SCHED_QUANTUM_TICKS 3 /* time slice: 30 ms */
#define THREAD_NAME_MAX 24

struct ipc_port;
struct process;

/* Callee-saved registers pushed by context_switch(); rip is the return address. */
struct context {
    uint64_t r15, r14, r13, r12, rbx, rbp, rip;
};

struct waitq {
    struct list_node list;
};

enum thread_state { T_READY, T_RUNNING, T_BLOCKED, T_SLEEPING, T_ZOMBIE };

struct thread {
    uint64_t tid;
    char name[THREAD_NAME_MAX];
    enum thread_state state;
    int prio;
    int slice; /* ticks left in the current quantum */

    struct context *ctx; /* saved stack pointer while not running */
    int kstack_slot;     /* -1 for the boot thread */
    uint64_t kstack_base;
    uint64_t kstack_top;  /* initial stack pointer; 0 for the boot thread */
    struct process *proc; /* user process this thread runs, or NULL for a kernel thread */

    struct list_node sched_node; /* run queue OR the wait queue it is blocked on */
    struct list_node sleep_node; /* timed sleep / timeout list */
    struct list_node all_node;   /* every live thread */

    struct waitq *wq; /* wait queue currently blocked on, if any */
    bool sleeping;    /* on the sleep list */
    bool timed_out;   /* last wait ended by timeout */
    uint64_t wake_tick;

    void (*entry)(void *);
    void *arg;
    int exit_code;
    bool detached;
    struct waitq joiners;

    struct ipc_port *reply_port; /* created lazily by ipc_call() */
    uint64_t cpu_ticks;
};

extern volatile int irq_depth;     /* >0 while handling an interrupt */
extern volatile bool need_resched; /* a higher-priority thread became ready / slice expired */

/* ---- lifecycle ---- */
void sched_init(void);  /* adopt the boot context as a thread and create the idle thread */
void sched_start(void); /* bring up APIC timer interrupts and enable interrupts */

struct thread *thread_create(const char *name, void (*entry)(void *), void *arg, int prio);
struct thread *thread_current(void);
__attribute__((noreturn)) void thread_exit(int code);
int thread_join(struct thread *t); /* waits, frees the thread, returns its exit code */
void thread_detach(struct thread *t);
int thread_set_priority(struct thread *t, int prio);

/* ---- time ---- */
void sched_yield(void);
void thread_sleep_ms(uint64_t ms);
void thread_sleep_ticks(uint64_t ticks);
uint64_t timer_ticks(void);
uint64_t ms_to_ticks(uint64_t ms);

/* ---- wait queues ----
 * The caller must have interrupts disabled (normally via spin_lock_irqsave on the lock
 * that protects the condition). waitq_wait() releases `lock` (may be NULL) while blocked
 * and re-acquires it before returning. Returns 0 when woken, -1 on timeout.
 * timeout_ticks == 0 waits forever.
 */
void waitq_init(struct waitq *q);
int waitq_wait(struct waitq *q, spinlock_t *lock, uint64_t timeout_ticks);
struct thread *waitq_wake_one(struct waitq *q);
void waitq_wake_all(struct waitq *q);

/* ---- introspection ---- */
uint64_t sched_context_switches(void);
uint32_t sched_thread_count(void);
void sched_dump(void);

/* ---- thread table (for IPC reply routing) ---- */
uint64_t thread_table_lock(void);
void thread_table_unlock(uint64_t flags);
struct thread *thread_lookup(uint64_t tid); /* table must be locked */

/* Called by schedule() just before switching to `next`: loads its address space and kernel stack.
 */
void sched_switch_hook(struct thread *next);

/* internal: called by the interrupt dispatcher */
void sched_irq_exit(void);

#endif

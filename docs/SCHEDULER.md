# Scheduling, Interrupts and IPC

Phase 3 design notes. Source: `src/kernel/sched.c`, `sync.c`, `ipc.c`, `kstack.c` and `src/arch/x86_64/{apic.c,idt.c,isr.asm,switch.asm}`.

## Interrupts

- All 256 vectors have an assembly stub (`isr.asm`) that builds a `struct interrupt_frame` and calls `interrupt_dispatch()`.
- Vectors 0-31 are CPU exceptions. They are fatal: a register dump is printed together with the current thread's name, then the machine halts.
- Vectors 32+ are IRQs. A handler is installed with `irq_register(vector, handler)`; it runs with interrupts disabled. The local APIC is acknowledged before the handler runs.
- The legacy 8259 PICs are remapped to 0x20-0x2F and fully masked. The local APIC replaces them. (The I/O APIC is not used yet; Phase 4 will need it for device interrupts.)
- The APIC registers are mapped uncached through `vmm_ioremap()`.
- The double-fault handler runs on its own IST stack, so a blown kernel stack is still reported.

## Timer

- The local APIC timer is calibrated against the PIT (channel 2, polled, 10 ms) once at boot, then runs in periodic mode.
- Tick rate: `TIMER_HZ` = 100 (10 ms). Time slice: `SCHED_QUANTUM_TICKS` = 3 (30 ms).

## Threads

| Item | Value |
|---|---|
| Kernel stack | 32 KiB mapped, one unmapped guard page below, in the region at `KSTACK_BASE` |
| Max threads | 512 (`KSTACK_MAX`) |
| States | `READY`, `RUNNING`, `BLOCKED`, `SLEEPING`, `ZOMBIE` |
| Exit | `thread_exit(code)` or returning from the entry function |
| Reclaim | `thread_join()` frees a thread and returns its code; `thread_detach()` lets the idle thread free it |

The boot context becomes thread 1 (`boot`). An `idle` thread (priority 7) always exists, so the scheduler always has something to run.

## Scheduler

- 8 priority levels, 0 is highest. Priority 7 is reserved for idle.
- **Strict priority:** a lower level only runs when every higher level is empty.
- **Round robin** within a level; a thread that uses up its slice goes to the back of its queue.
- **Preemption:** the timer sets `need_resched` when a slice expires or when a wakeup readies a thread with a higher priority than the running one. The switch happens on interrupt exit (or at the next `irq_restore()` in thread context).
- `context_switch()` saves only callee-saved registers on the old stack and swaps stack pointers. Everything else is already on the stack from the interrupt frame or the C calling convention.
- Run queues are bitmap-indexed, so picking the next thread is O(1).

## Locking rules

- **Uniprocessor model.** Scheduler state is protected by disabling interrupts. Spinlocks are real atomic locks (SMP-ready) but on one CPU the `irqsave` part is what provides exclusion.
- `spin_lock_irqsave()` / `spin_unlock_irqrestore()` is the default pair. Restoring interrupts also honours a pending reschedule.
- Taking a spinlock never blocks and must not be held across a sleep. A spin that never ends is reported as a deadlock instead of hanging the machine.
- **Lock order:** heap → vmm → pmm. Never take them in another order.
- `mutex` and `semaphore` sleep, so they are for thread context only. Mutexes are not recursive and have no priority inheritance yet.
- Blocking is built on `waitq_wait(queue, lock, timeout)`: it queues the thread, releases `lock`, switches away, and re-takes `lock` on wakeup. Because interrupts stay off from "decide to block" to "switch away", a wakeup cannot be lost.

## IPC

- A **port** is a bounded ring buffer of fixed-size messages (`struct ipc_msg`: type, length, sender, 48 bytes of payload). Messages are copied, so there is no ownership to track.
- `ipc_send` / `ipc_recv` block when the port is full / empty. The `_timeout` forms take milliseconds (`0` = never block, `IPC_FOREVER` = wait indefinitely).
- The kernel stamps `sender` with the sender's thread id; it cannot be forged by the payload.
- `ipc_call()` / `ipc_reply()` give synchronous request/reply using a per-thread reply port created on first use. A late reply to a call that already timed out is discarded.
- `ipc_port_destroy()` wakes every blocked thread with `IPC_ECLOSED`; the last thread to leave frees the port, so no thread is ever left using freed memory.

## Known limitations

- Single CPU: no AP startup, no per-CPU data, no TLB shootdowns (Phase 7).
- No priority inheritance, so priority inversion is possible with mutexes.
- The sleep list is scanned linearly on every tick. Fine for hundreds of threads; a timer wheel can replace it later.
- Messages are limited to 48 bytes. Large transfers will need shared memory.
- Exceptions are still fatal; page-fault recovery and user-mode faults come with Phase 6.

## Testing

`sched_selftest()` runs 179 checks on every boot: interrupt-flag handling, sleep accuracy, thread create/join/detach and leak checks (frames and heap), stack guard pages, preemption of CPU hogs, fair sharing, strict priorities, preemption by a waking high-priority thread, mutex exclusion and lost-update checks, a semaphore producer/consumer, IPC ordering / timeouts / request-reply / destroy-while-blocked, and a 4-thread heap stress test under preemption. `make test` requires the `SCHED SELFTEST: PASS` line.

Also checked by hand: a stack overflow inside a spawned thread trips its guard page and is reported as a double fault naming the thread; a null dereference in a thread reports the thread; unlocking a mutex you do not own panics.

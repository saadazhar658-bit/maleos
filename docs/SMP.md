# Multi-core support (Phase 7c)

Maleos starts every CPU the ACPI MADT lists (up to `SMP_MAX` = 8) and schedules threads on
all of them. **Kernel code still runs one CPU at a time**: a single *big kernel lock* (BKL)
covers all of it. User programs run truly in parallel.

Sources: `src/arch/x86_64/smp.c`, `smp.asm` (start-up trampoline, idle wait), `gdt.c`,
`isr.asm`, `syscall.asm`; scheduler in `src/kernel/sched.c`; tests in
`src/kernel/smp_selftest.c` and `user/test/t_smp.c`.

## Bring-up
1. The boot CPU parses the MADT, then for each other CPU copies a real-mode trampoline to
   physical page 0x8000, fills in its data block (stack, per-CPU pointer, kernel CR3) and
   sends INIT + two SIPIs.
2. The AP climbs real → protected → long mode on the boot page tables, loads the kernel CR3
   and calls `ap_main()`: own GDT/TSS, shared IDT, SYSCALL MSRs, local APIC and timer.
3. It takes the BKL, becomes the idle thread of its CPU and starts ticking at 100 Hz.

## Per-CPU data
Each CPU's GS base points at its `struct cpu` (`arch/percpu.h`): current and idle thread,
interrupt nesting, reschedule flag, TSS and GDT, statistics. `this_cpu()` is one `mov %gs:0`.
SWAPGS flips the user and kernel GS bases on every ring transition (`isr.asm`, `syscall.asm`,
`enter_user`).

## The big kernel lock
A ticket lock (FIFO, nobody starves). A CPU takes it on every entry from user mode
(interrupt, exception, SYSCALL) and drops it on the way back to user mode. The idle loop
drops it around `sti; hlt`; interrupts landing exactly there are recognised by instruction
address (`smp_frame_unlocked`). Switching threads on a CPU keeps the lock, so it belongs to
the CPU's kernel-mode execution, not to a thread, and a thread may resume on any CPU.

Consequences, stated plainly:
- All existing "interrupts off" critical sections stay correct without an audit.
- Kernel threads effectively share one core: whichever CPU holds the lock runs whatever is
  ready. Other CPUs help only with user-mode work (and kernel work when the holder leaves).
- Fine-grained locking is the obvious next step; the BKL is the thing to remove.

## Scheduler
One global run queue. Each CPU has its own idle thread that is never queued. When a thread
becomes ready, an idle CPU is picked (the current one if it is idle, otherwise a random
idle one, woken with a reschedule IPI, vector 0xFD); failing that the CPU running the
lowest-priority thread is asked to preempt. The boot CPU keeps the clock and the sleep list.
A safety net lets an idle CPU pick up work that waited two ticks.
`sched_set_cpu_limit()` decides how many CPUs may run threads; the early self-tests run with
one, then `smp_selftest` raises it.

## TLB
Kernel mappings that are removed or changed bump a global generation counter; every CPU
compares it when it takes the BKL and flushes if it is behind. No IPI is needed because a
CPU runs no kernel code without the lock. User address spaces are used by one thread at a
time and are flushed by the CR3 switch.

## Limits
- Kernel code is serialised by one lock (see above).
- No CPU hotplug, no x2APIC, no NUMA, no CPU affinity, no per-CPU run queues.
- A kernel panic on one CPU does not stop the others.
- QEMU only: tested with `-smp 1..4` (TCG).

## Testing
`SMP=n ./scripts/boot-test.sh` (default 4), `shell-test.py --smp n`, `net-test.py --smp n`.
`SMP SELFTEST` checks bring-up, IPIs, per-CPU timers and thread churn; `t_smp` proves
user-mode parallelism by comparing user-mode ticks with wall-clock ticks; the syscall fuzzer
and network tests run on all CPUs. `cpus` shows per-CPU statistics.

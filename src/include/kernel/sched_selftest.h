#ifndef MALEOS_KERNEL_SCHED_SELFTEST_H
#define MALEOS_KERNEL_SCHED_SELFTEST_H

/* Exercises timer, scheduler, locks and IPC. Panics on failure; returns the check count. */
int sched_selftest(void);

#endif

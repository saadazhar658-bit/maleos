#ifndef MALEOS_KERNEL_SMP_SELFTEST_H
#define MALEOS_KERNEL_SMP_SELFTEST_H

/*
 * Waits for the other CPUs, lets the scheduler use them, and checks that threads really move
 * between CPUs, that IPIs and per-CPU timers work and that kernel mappings stay coherent.
 * `ncpu` is what smp_init() reported. Panics on failure; returns the check count.
 */
int smp_selftest(int ncpu);

#endif

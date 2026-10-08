#ifndef MALEOS_ARCH_SYSCALL_H
#define MALEOS_ARCH_SYSCALL_H

#include <stdbool.h>
#include <stdint.h>

/* Enable the SYSCALL instruction (and SMEP/SMAP where the CPU supports them). */
void syscall_init(void);

bool syscall_smep_enabled(void);
bool syscall_smap_enabled(void);

/* Stack top used by the syscall entry stub; written by the scheduler on every switch. */
extern uint64_t syscall_kstack_top;

#endif

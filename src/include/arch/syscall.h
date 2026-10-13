#ifndef MALEOS_ARCH_SYSCALL_H
#define MALEOS_ARCH_SYSCALL_H

#include <stdbool.h>
#include <stdint.h>

/* Enable the SYSCALL instruction (per CPU: every CPU calls this) (and SMEP/SMAP where the CPU
 * supports them). */
void syscall_init(void);

bool syscall_smep_enabled(void);
bool syscall_smap_enabled(void);

#endif

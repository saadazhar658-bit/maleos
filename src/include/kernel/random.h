#ifndef KERNEL_RANDOM_H
#define KERNEL_RANDOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Seeds the generator (RDRAND when available, otherwise the TSC) and installs a random
 * stack-protector canary. Must run once, early in boot. */
void random_init(void);

/* Fast general-purpose generator (xoshiro256**). Good for ASLR and canaries; it is not a
 * cryptographic generator. */
uint64_t random_u64(void);
void random_bytes(void *buf, size_t len);

/* Returns true if the hardware RNG (RDRAND) fed the seed. */
bool random_hw_seeded(void);

/* Stack-protector canary read by compiler-generated code. */
extern uintptr_t __stack_chk_guard;

#endif

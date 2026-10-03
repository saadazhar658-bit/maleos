#ifndef MALEOS_ARCH_APIC_H
#define MALEOS_ARCH_APIC_H

#include <stdint.h>

#define VEC_TIMER 0x30
#define VEC_SPURIOUS 0xFF

/* Remap and mask the legacy 8259 PICs, enable the local APIC. Panics if there is none. */
void apic_init(void);
/* Measure the APIC timer against the PIT, then run it periodically at `hz`. */
void apic_timer_start(uint32_t hz);
void apic_eoi(void);
uint32_t apic_id(void);
uint32_t apic_timer_ticks_per_10ms(void);

#endif

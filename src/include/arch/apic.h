#ifndef MALEOS_ARCH_APIC_H
#define MALEOS_ARCH_APIC_H

#include <stdint.h>

#define VEC_TIMER 0x30
#define VEC_RESCHED 0xFD /* IPI: wake an idle CPU / ask it to reschedule */
#define VEC_SPURIOUS 0xFF

/* Remap and mask the legacy 8259 PICs, enable the local APIC. Panics if there is none. */
void apic_init(void);
/* Measure the APIC timer against the PIT, then run it periodically at `hz`. */
void apic_timer_start(uint32_t hz);
void apic_eoi(void);
uint32_t apic_id(void);
uint32_t apic_timer_ticks_per_10ms(void);

/* Application processors: enable this CPU's local APIC, then start its (already calibrated) timer.
 */
void apic_init_ap(void);
void apic_timer_start_ap(uint32_t hz);
/* Inter-processor interrupts. */
void apic_send_ipi(uint32_t dest_apic_id, uint8_t vector);
void apic_send_init(uint32_t dest_apic_id);
void apic_send_sipi(uint32_t dest_apic_id, uint8_t page); /* start at physical page << 12 */
/* Crude busy-wait (port 0x80 writes, about 1 us each); usable before any timer runs. */
void apic_delay_us(uint32_t us);

#endif

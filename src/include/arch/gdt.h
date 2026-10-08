#ifndef MALEOS_ARCH_GDT_H
#define MALEOS_ARCH_GDT_H

#include <stdint.h>

/*
 * Segment layout. The order of the user entries is dictated by SYSRET/STAR: with the base
 * selector 0x18, SYSRET loads SS = base + 8 and CS = base + 16.
 */
#define KERNEL_CS 0x08
#define KERNEL_DS 0x10
#define USER_CS32 0x18 /* unused placeholder required by SYSRET's layout */
#define USER_DS (0x20 | 3)
#define USER_CS (0x28 | 3)
#define TSS_SEL 0x30

void gdt_init(void);

/* Kernel stack the CPU switches to when an interrupt or exception arrives from ring 3. */
void gdt_set_rsp0(uint64_t rsp0);

#endif

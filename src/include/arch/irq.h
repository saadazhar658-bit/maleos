#ifndef MALEOS_ARCH_IRQ_H
#define MALEOS_ARCH_IRQ_H

#include "arch/idt.h"

typedef void (*irq_handler_t)(struct interrupt_frame *frame);

/* Install a handler for an interrupt vector (32..254). It runs with interrupts disabled. */
void irq_register(int vector, irq_handler_t handler);

#endif

#ifndef MALEOS_ARCH_IDT_H
#define MALEOS_ARCH_IDT_H

#include <stdint.h>

/* Register state pushed by isr_common (see isr.asm). */
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
};

void idt_init(void);
void idt_load(void); /* load the shared IDT on this CPU */

#endif

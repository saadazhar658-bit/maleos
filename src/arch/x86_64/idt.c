#include "arch/idt.h"

#include <stdint.h>

#include "arch/apic.h"
#include "arch/cpu.h"
#include "arch/gdt.h"
#include "arch/irq.h"
#include "kernel/printk.h"
#include "kernel/sched.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

#define IDT_INTERRUPT_GATE 0x8E /* present, DPL 0, 64-bit interrupt gate */

extern uint64_t isr_stub_table[256];

static struct idt_entry idt[256] __attribute__((aligned(16)));
static irq_handler_t irq_handlers[256];

static const char *const exception_names[32] = {
    "Divide Error",
    "Debug",
    "Non-Maskable Interrupt",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack-Segment Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 Floating-Point",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating-Point",
    "Virtualization",
    "Control Protection",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection",
    "VMM Communication",
    "Security",
    "Reserved",
};

static void set_gate(int vec, uint64_t handler, uint8_t ist)
{
    idt[vec].offset_low = handler & 0xFFFF;
    idt[vec].selector = KERNEL_CS;
    idt[vec].ist = ist;
    idt[vec].type_attr = IDT_INTERRUPT_GATE;
    idt[vec].offset_mid = (handler >> 16) & 0xFFFF;
    idt[vec].offset_high = handler >> 32;
    idt[vec].zero = 0;
}

void idt_init(void)
{
    for (int i = 0; i < 256; i++)
        set_gate(i, isr_stub_table[i], i == 8 ? 1 : 0); /* double fault runs on IST1 */

    struct idt_ptr ptr = {.limit = sizeof(idt) - 1, .base = (uint64_t)idt};
    __asm__ volatile("lidt %0" : : "m"(ptr));
}

void irq_register(int vector, irq_handler_t handler)
{
    if (vector < 32 || vector > 254)
        kpanic("irq_register: bad vector %d", vector);
    irq_handlers[vector] = handler;
}

static void report_exception(struct interrupt_frame *f)
{
    printk_enter_panic();
    printk("\n*** EXCEPTION %lu: %s ***\n", (unsigned long)f->vector,
           f->vector < 32 ? exception_names[f->vector] : "Unknown");
    printk("error=0x%lx  rip=%016lx  cs=%lx  rflags=%lx\n", (unsigned long)f->error,
           (unsigned long)f->rip, (unsigned long)f->cs, (unsigned long)f->rflags);
    printk("rsp=%016lx  ss=%lx  cr3=%016lx\n", (unsigned long)f->rsp, (unsigned long)f->ss,
           (unsigned long)read_cr3());

    if (f->vector == 14) {
        uint64_t e = f->error;
        printk("cr2=%016lx  (%s, %s, %s%s)\n", (unsigned long)read_cr2(),
               (e & 1) ? "protection violation" : "page not present", (e & 2) ? "write" : "read",
               (e & 4) ? "user" : "kernel", (e & 16) ? ", instruction fetch" : "");
    }

    printk("rax=%016lx rbx=%016lx rcx=%016lx rdx=%016lx\n", (unsigned long)f->rax,
           (unsigned long)f->rbx, (unsigned long)f->rcx, (unsigned long)f->rdx);
    printk("rsi=%016lx rdi=%016lx rbp=%016lx\n", (unsigned long)f->rsi, (unsigned long)f->rdi,
           (unsigned long)f->rbp);

    struct thread *t = thread_current();
    if (t)
        printk("thread: %s (tid %lu)\n", t->name, (unsigned long)t->tid);
}

/* Called from isr_common for every vector. CPU exceptions are fatal; the rest are IRQs. */
void interrupt_dispatch(struct interrupt_frame *f)
{
    if (f->vector < 32) {
        report_exception(f);
        halt_forever();
    }

    if (f->vector == VEC_SPURIOUS)
        return; /* spurious interrupts must not be acknowledged */

    irq_depth++;
    apic_eoi(); /* ack first: handlers run with interrupts off, and a context switch may follow */

    irq_handler_t h = irq_handlers[f->vector];
    if (h) {
        h(f);
    } else {
        static int reported;
        if (reported++ < 5)
            printk("unexpected interrupt, vector %lu\n", (unsigned long)f->vector);
    }

    irq_depth--;
    if (irq_depth == 0 && need_resched)
        sched_irq_exit();
}

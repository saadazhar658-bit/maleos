#include "arch/idt.h"

#include <stdint.h>

#include "arch/cpu.h"
#include "arch/gdt.h"
#include "kernel/printk.h"

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

extern uint64_t isr_stub_table[32];

static struct idt_entry idt[256] __attribute__((aligned(16)));

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
    for (int i = 0; i < 32; i++)
        set_gate(i, isr_stub_table[i], i == 8 ? 1 : 0); /* double fault runs on IST1 */

    struct idt_ptr ptr = {.limit = sizeof(idt) - 1, .base = (uint64_t)idt};
    __asm__ volatile("lidt %0" : : "m"(ptr));
}

/* Called from isr_common. All exceptions are fatal for now. */
void exception_handler(struct interrupt_frame *f)
{
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

    halt_forever();
}

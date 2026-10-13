#include "arch/gdt.h"

#include <stdint.h>

#include "arch/cpu.h"
#include "arch/percpu.h"
#include "mm/mm.h"

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

extern char __stack_top[];

#define MSR_GS_BASE 0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

struct cpu cpus[SMP_MAX];

/* Build and load this CPU's GDT and TSS, and point GS at its per-CPU data. The convention
 * (see isr.asm): in kernel mode GS_BASE is the per-CPU pointer, KERNEL_GS_BASE holds the
 * user value (0); SWAPGS flips them on every ring transition. */
void gdt_init_cpu(struct cpu *c, uint64_t rsp0)
{
    uint64_t *gdt = c->gdt;
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL; /* 64-bit code, DPL 0 */
    gdt[2] = 0x00CF92000000FFFFULL; /* data, DPL 0 */
    gdt[3] = 0x00CFFA000000FFFFULL; /* 32-bit code, DPL 3 (only fills SYSRET's slot) */
    gdt[4] = 0x00CFF2000000FFFFULL; /* data, DPL 3 */
    gdt[5] = 0x00AFFA000000FFFFULL; /* 64-bit code, DPL 3 */

    c->tss.rsp[0] = rsp0;
    c->tss.ist[0] = (uint64_t)(c->df_stack + sizeof(c->df_stack)); /* IST1 */
    c->tss.iomap_base = sizeof(c->tss);
    c->kstack_top = rsp0;
    c->self = c;

    uint64_t base = (uint64_t)&c->tss;
    uint64_t limit = sizeof(c->tss) - 1;
    gdt[6] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[7] = base >> 32;

    struct gdt_ptr ptr = {.limit = sizeof(c->gdt) - 1, .base = (uint64_t)gdt};
    __asm__ volatile("lgdt %0" : : "m"(ptr));

    /* Reload CS with a far return, then the data segments. */
    __asm__ volatile("pushq %0\n\t"
                     "leaq 1f(%%rip), %%rax\n\t"
                     "pushq %%rax\n\t"
                     "lretq\n"
                     "1:\n\t"
                     "mov %1, %%ax\n\t"
                     "mov %%ax, %%ds\n\t"
                     "mov %%ax, %%es\n\t"
                     "mov %%ax, %%ss\n\t"
                     "xor %%eax, %%eax\n\t"
                     "mov %%ax, %%fs\n\t"
                     "mov %%ax, %%gs\n\t"
                     :
                     : "i"(KERNEL_CS), "i"(KERNEL_DS)
                     : "rax", "memory");

    __asm__ volatile("ltr %0" : : "r"((uint16_t)TSS_SEL));

    /* Loading %gs cleared its base; install the per-CPU pointer afterwards. */
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

void gdt_init(void)
{
    gdt_init_cpu(&cpus[0], (uint64_t)__stack_top);
}

void gdt_set_rsp0(uint64_t rsp0)
{
    struct cpu *c = this_cpu();
    c->tss.rsp[0] = rsp0;
    c->kstack_top = rsp0;
}

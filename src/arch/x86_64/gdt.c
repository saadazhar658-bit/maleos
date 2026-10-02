#include "arch/gdt.h"

#include <stdint.h>

#include "mm/mm.h"

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* null, kernel code, kernel data, TSS (two slots) */
static uint64_t gdt[5] __attribute__((aligned(16)));
static struct tss tss;

/* Dedicated stack for double faults so a blown kernel stack is still reported. */
static uint8_t df_stack[8192] __attribute__((aligned(16)));

extern char __stack_top[];

void gdt_init(void)
{
    gdt[0] = 0;
    gdt[1] = 0x00AF9A000000FFFFULL; /* 64-bit code, DPL 0 */
    gdt[2] = 0x00CF92000000FFFFULL; /* data, DPL 0 */

    tss.rsp[0] = (uint64_t)__stack_top;
    tss.ist[0] = (uint64_t)(df_stack + sizeof(df_stack)); /* IST1 */
    tss.iomap_base = sizeof(tss);

    uint64_t base = (uint64_t)&tss;
    uint64_t limit = sizeof(tss) - 1;
    gdt[3] = (limit & 0xFFFF) | ((base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
             (((limit >> 16) & 0xF) << 48) | (((base >> 24) & 0xFF) << 56);
    gdt[4] = base >> 32;

    struct gdt_ptr ptr = {.limit = sizeof(gdt) - 1, .base = (uint64_t)gdt};
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
}

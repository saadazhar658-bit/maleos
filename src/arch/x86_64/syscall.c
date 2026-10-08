#include "arch/syscall.h"

#include "arch/cpu.h"
#include "arch/gdt.h"
#include "kernel/printk.h"

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_FMASK 0xC0000084
#define EFER_SCE (1ULL << 0)

#define CR4_SMEP (1ULL << 20)
#define CR4_SMAP (1ULL << 21)

#define FM_TF 0x100
#define FM_IF 0x200
#define FM_DF 0x400
#define FM_AC 0x40000

extern void syscall_entry(void);

/* Kernel stack of the running thread; maintained by sched_switch_hook(). */
uint64_t syscall_kstack_top;

static bool smep_on, smap_on;

void syscall_init(void)
{
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    /* SYSCALL: CS = STAR[47:32], SS = that + 8. SYSRET (unused, see syscall.asm) would use [63:48].
     */
    wrmsr(MSR_STAR, ((uint64_t)USER_CS32 << 48) | ((uint64_t)KERNEL_CS << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    /* Entry runs with interrupts off, direction flag clear, no single-stepping, no AC. */
    wrmsr(MSR_FMASK, FM_IF | FM_DF | FM_TF | FM_AC);

    /*
     * Supervisor-mode protections, when the CPU has them. The kernel never dereferences user
     * pointers (it copies through the direct map after walking the page tables), so both can
     * stay on permanently.
     */
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    if (a >= 7) {
        cpuid(7, &a, &b, &c, &d);
        uint64_t cr4 = read_cr4();
        if (b & (1u << 7)) {
            cr4 |= CR4_SMEP;
            smep_on = true;
        }
        if (b & (1u << 20)) {
            cr4 |= CR4_SMAP;
            smap_on = true;
        }
        write_cr4(cr4);
    }
}

bool syscall_smep_enabled(void)
{
    return smep_on;
}

bool syscall_smap_enabled(void)
{
    return smap_on;
}

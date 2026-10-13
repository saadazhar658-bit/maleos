#include "kernel/smp.h"
#include "arch/apic.h"
#include "arch/cpu.h"
#include "arch/gdt.h"
#include "arch/idt.h"
#include "arch/irq.h"
#include "arch/percpu.h"
#include "arch/syscall.h"
#include "kernel/acpi.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/string.h"
#include "mm/mm.h"
#include "mm/vmm.h"

extern char ap_tramp_start[], ap_tramp_end[];
extern char ap_data_pml4[], ap_data_stack[], ap_data_cpu[], ap_data_entry[], ap_data_cr3[];
extern char ap_high[];
extern char boot_pml4[];
extern char idle_window_start[], idle_window_end[];

#define TRAMP_PHYS 0x8000

int cpu_count = 1;

/* ---- the big kernel lock (ticket lock: FIFO, so no CPU is starved) ---- */

static struct {
    volatile uint32_t next;
    volatile uint32_t serving;
    volatile int owner;
} bkl = {1, 0, 0}; /* the boot CPU holds it from the start */

static volatile uint64_t tlb_gen;

static void tlb_sync(void)
{
    struct cpu *c = this_cpu();
    uint64_t g = __atomic_load_n(&tlb_gen, __ATOMIC_ACQUIRE);
    if (c->tlb_seen != g) {
        c->tlb_seen = g;
        write_cr3(read_cr3()); /* no global pages are used: this flushes everything */
    }
}

void bkl_lock(void)
{
    uint32_t my = __atomic_fetch_add(&bkl.next, 1, __ATOMIC_RELAXED);
    while (__atomic_load_n(&bkl.serving, __ATOMIC_ACQUIRE) != my)
        __builtin_ia32_pause();
    bkl.owner = (int)this_cpu()->id;
    tlb_sync();
}

void bkl_unlock(void)
{
    bkl.owner = -1;
    __atomic_store_n(&bkl.serving, bkl.serving + 1, __ATOMIC_RELEASE);
}

bool bkl_held(void)
{
    return bkl.owner == (int)this_cpu()->id && bkl.serving != bkl.next;
}

bool smp_frame_unlocked(uint64_t cs, uint64_t rip)
{
    return (cs & 3) == 3 || rip == (uint64_t)idle_window_start || rip == (uint64_t)idle_window_end;
}

void smp_tlb_bump(void)
{
    __atomic_add_fetch(&tlb_gen, 1, __ATOMIC_RELEASE);
}

/* ---- bring-up ---- */

static uint8_t ap_stacks[SMP_MAX][AP_STACK_SIZE] __attribute__((aligned(16)));

void ap_main(struct cpu *c)
{
    gdt_init_cpu(c, (uint64_t)ap_stacks[c->id] + AP_STACK_SIZE);
    idt_load();
    syscall_init();
    apic_init_ap();
    c->started = true;

    bkl_lock(); /* from here on we are ordinary kernel code */
    sched_ap_enter(c);
}

static void start_ap(struct cpu *c)
{
    uint8_t *tramp = (uint8_t *)(HHDM_BASE + TRAMP_PHYS);
    memcpy(tramp, ap_tramp_start, (size_t)(ap_tramp_end - ap_tramp_start));

#define DATA(sym) ((volatile uint64_t *)(tramp + (sym - ap_tramp_start)))
    *DATA(ap_data_pml4) = (uint64_t)boot_pml4;
    *DATA(ap_data_stack) = (uint64_t)ap_stacks[c->id] + AP_STACK_SIZE;
    *DATA(ap_data_cpu) = (uint64_t)c;
    *DATA(ap_data_entry) = (uint64_t)ap_high;
    *DATA(ap_data_cr3) = vmm_kernel_pml4();
#undef DATA

    apic_send_init(c->apic_id);
    apic_delay_us(10000);
    apic_send_sipi(c->apic_id, TRAMP_PHYS >> 12);
    apic_delay_us(200);
    apic_send_sipi(c->apic_id, TRAMP_PHYS >> 12);

    for (int i = 0; i < 2000 && !c->started; i++)
        apic_delay_us(1000);
}

static void resched_ipi(struct interrupt_frame *f)
{
    (void)f;
    struct cpu *c = this_cpu();
    c->ipis++;
    c->resched = true;
}

int smp_init(void)
{
    const struct acpi_info *a = acpi_get();
    cpus[0].id = 0;
    cpus[0].apic_id = apic_id();

    irq_register(VEC_RESCHED, resched_ipi);

    int found = 1;
    if (a->present) {
        for (int i = 0; i < a->cpu_count && found < SMP_MAX; i++) {
            if (a->cpu_apic_ids[i] == cpus[0].apic_id)
                continue;
            struct cpu *c = &cpus[found];
            c->id = (uint32_t)found;
            c->apic_id = a->cpu_apic_ids[i];
            sched_prepare_ap(c, (uint64_t)ap_stacks[found] + AP_STACK_SIZE);
            start_ap(c);
            if (!c->started) {
                printk("SMP: CPU with APIC id %u did not start\n", c->apic_id);
                continue;
            }
            found++;
        }
    }
    return found;
}

int smp_online_count(void)
{
    int n = 0;
    for (int i = 0; i < SMP_MAX; i++)
        if (cpus[i].online)
            n++;
    return n;
}

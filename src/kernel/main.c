#include <stdint.h>

#include "arch/apic.h"
#include "arch/cpu.h"
#include "arch/gdt.h"
#include "arch/idt.h"
#include "arch/serial.h"
#include "arch/vga.h"
#include "kernel/bootinfo.h"
#include "kernel/printk.h"
#include "kernel/sched.h"
#include "kernel/sched_selftest.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/selftest.h"
#include "mm/vmm.h"

static struct boot_info boot_info;

static const char *region_type_name(uint32_t type)
{
    switch (type) {
    case MEM_AVAILABLE:
        return "usable";
    case MEM_RESERVED:
        return "reserved";
    case MEM_ACPI_RECLAIMABLE:
        return "ACPI reclaimable";
    case MEM_ACPI_NVS:
        return "ACPI NVS";
    case MEM_BAD:
        return "bad";
    default:
        return "unknown";
    }
}

static void print_memory_map(void)
{
    printk("Physical memory map:\n");
    for (size_t i = 0; i < boot_info.region_count; i++) {
        const struct mem_region *r = &boot_info.regions[i];
        printk("  [%016lx - %016lx) %s\n", (unsigned long)r->base,
               (unsigned long)(r->base + r->length), region_type_name(r->type));
    }
}

void kmain(uint64_t mbi_phys)
{
    /* Phase 1 essentials: output, segmentation, exception handling. */
    serial_init();
    vga_init();
    printk("Maleos kernel starting\n");

    gdt_init();
    idt_init();
    printk("GDT/IDT loaded\n");

    /* Phase 2: memory management. */
    bootinfo_parse(mbi_phys, &boot_info);
    print_memory_map();

    pmm_init(&boot_info);
    printk("PMM: %lu MiB usable, %lu MiB free (%lu frames)\n",
           (unsigned long)(pmm_total_frames() * PAGE_SIZE >> 20),
           (unsigned long)(pmm_free_frame_count() * PAGE_SIZE >> 20),
           (unsigned long)pmm_free_frame_count());

    vmm_init(&boot_info);
    vga_set_base((uint16_t *)(HHDM_BASE + 0xB8000)); /* identity map is gone */
    printk("VMM: kernel address space active (cr3=%lx), W^X enforced\n",
           (unsigned long)vmm_kernel_pml4());

    heap_init();
    struct heap_stats hs;
    heap_get_stats(&hs);
    printk("Heap: %lu KiB mapped at %lx\n", (unsigned long)(hs.mapped_bytes >> 10),
           (unsigned long)KHEAP_BASE);

    printk("MALEOS BOOT OK\n");

    int n = mm_selftest();
    printk("MM SELFTEST: PASS (%d checks)\n", n);
    printk("PMM: %lu frames free after tests\n", (unsigned long)pmm_free_frame_count());

    /* Phase 3: interrupts, timer, scheduler. */
    sched_init();
    sched_start();
    printk("APIC: id %u, timer %u ticks per 10 ms, %d Hz scheduler tick\n", apic_id(),
           apic_timer_ticks_per_10ms(), TIMER_HZ);
    printk("Scheduler: %d priority levels, %d ms quantum, preemptive\n", SCHED_PRIO_LEVELS,
           SCHED_QUANTUM_TICKS * 1000 / TIMER_HZ);

    int n3 = sched_selftest();
    printk("SCHED SELFTEST: PASS (%d checks, %lu context switches)\n", n3,
           (unsigned long)sched_context_switches());
    sched_dump();

    qemu_debug_exit(0x10); /* no-op unless QEMU has isa-debug-exit (used by `make test`) */

    /* Normal runs: stay alive and show that the timer and scheduler keep working. */
    for (;;) {
        thread_sleep_ms(5000);
        printk("uptime: %lu s\n", (unsigned long)(timer_ticks() / TIMER_HZ));
    }
}

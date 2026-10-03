#include "arch/apic.h"

#include "arch/cpu.h"
#include "kernel/printk.h"
#include "mm/vmm.h"

#define MSR_APIC_BASE 0x1B
#define APIC_BASE_ENABLE (1ULL << 11)

#define REG_ID 0x020
#define REG_TPR 0x080
#define REG_EOI 0x0B0
#define REG_SVR 0x0F0
#define REG_LVT_TIMER 0x320
#define REG_LVT_LINT0 0x350
#define REG_LVT_LINT1 0x360
#define REG_LVT_ERROR 0x370
#define REG_TIMER_INIT 0x380
#define REG_TIMER_CUR 0x390
#define REG_TIMER_DIV 0x3E0

#define LVT_MASKED (1u << 16)
#define LVT_PERIODIC (1u << 17)
#define SVR_ENABLE (1u << 8)
#define TIMER_DIV_16 0x3

static volatile uint32_t *lapic;
static uint32_t ticks_per_10ms;

static inline uint32_t reg_read(uint32_t reg)
{
    return lapic[reg / 4];
}

static inline void reg_write(uint32_t reg, uint32_t value)
{
    lapic[reg / 4] = value;
}

static void io_wait(void)
{
    outb(0x80, 0);
}

/* Remap the 8259s out of the exception range, then mask every line. */
static void pic_disable(void)
{
    outb(0x20, 0x11);
    io_wait();
    outb(0xA0, 0x11);
    io_wait();
    outb(0x21, 0x20); /* master vectors 0x20-0x27 */
    io_wait();
    outb(0xA1, 0x28); /* slave vectors 0x28-0x2F */
    io_wait();
    outb(0x21, 0x04);
    io_wait();
    outb(0xA1, 0x02);
    io_wait();
    outb(0x21, 0x01);
    io_wait();
    outb(0xA1, 0x01);
    io_wait();
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
}

void apic_init(void)
{
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    if (!(d & (1u << 9)))
        kpanic("CPU has no local APIC");

    pic_disable();

    uint64_t base = rdmsr(MSR_APIC_BASE);
    if (!(base & APIC_BASE_ENABLE))
        wrmsr(MSR_APIC_BASE, base | APIC_BASE_ENABLE);

    lapic = vmm_ioremap(base & 0xFFFFFFFFFFFFF000ULL, 4096);
    if (!lapic)
        kpanic("cannot map the local APIC");

    reg_write(REG_TPR, 0); /* accept every priority */
    reg_write(REG_LVT_TIMER, LVT_MASKED);
    reg_write(REG_LVT_LINT0, LVT_MASKED);
    reg_write(REG_LVT_LINT1, LVT_MASKED);
    reg_write(REG_LVT_ERROR, LVT_MASKED);
    reg_write(REG_SVR, SVR_ENABLE | VEC_SPURIOUS);
}

uint32_t apic_id(void)
{
    return reg_read(REG_ID) >> 24;
}

void apic_eoi(void)
{
    reg_write(REG_EOI, 0);
}

uint32_t apic_timer_ticks_per_10ms(void)
{
    return ticks_per_10ms;
}

/* Count APIC timer ticks during 10 ms of PIT channel 2 (mode 0, polled). */
static uint32_t calibrate_timer(void)
{
    const uint16_t pit_count = 11932; /* 1193182 Hz / 100 */

    reg_write(REG_TIMER_DIV, TIMER_DIV_16);
    reg_write(REG_LVT_TIMER, LVT_MASKED);

    uint8_t gate = inb(0x61) & 0xFC; /* gate off, speaker off */
    outb(0x61, gate);
    outb(0x43, 0xB0); /* channel 2, lobyte/hibyte, mode 0 */
    outb(0x42, pit_count & 0xFF);
    outb(0x42, pit_count >> 8);

    outb(0x61, gate | 0x01); /* open the gate: counting starts */
    reg_write(REG_TIMER_INIT, 0xFFFFFFFF);

    unsigned long guard = 0;
    while (!(inb(0x61) & 0x20)) { /* OUT2 goes high at terminal count */
        if (++guard > 200000000UL)
            return 0;
    }

    uint32_t remaining = reg_read(REG_TIMER_CUR);
    reg_write(REG_TIMER_INIT, 0); /* stop */
    outb(0x61, gate);
    return 0xFFFFFFFFu - remaining;
}

void apic_timer_start(uint32_t hz)
{
    uint32_t per10ms = calibrate_timer();
    if (per10ms < 1000) {
        printk("APIC: timer calibration failed (%u), using a default rate\n", per10ms);
        per10ms = 1000000;
    }
    ticks_per_10ms = per10ms;

    uint32_t initial = (uint32_t)(((uint64_t)per10ms * 100) / hz);
    reg_write(REG_TIMER_DIV, TIMER_DIV_16);
    reg_write(REG_LVT_TIMER, VEC_TIMER | LVT_PERIODIC);
    reg_write(REG_TIMER_INIT, initial);
}

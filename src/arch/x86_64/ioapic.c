#include "arch/ioapic.h"

#include "arch/apic.h"
#include "kernel/printk.h"
#include "kernel/spinlock.h"
#include "mm/vmm.h"

#define IOAPIC_DEFAULT_BASE 0xFEC00000u

#define IOREGSEL 0x00
#define IOWIN 0x10
#define REG_VER 0x01
#define REG_REDIR(n) (0x10 + 2 * (n))

#define RED_MASKED (1u << 16)
#define RED_LEVEL (1u << 15)
#define RED_ACTIVE_LOW (1u << 13)

struct ioapic {
    volatile uint32_t *regs;
    uint32_t gsi_base;
    uint32_t pins;
    uint8_t id;
};

static struct ioapic ioapics[ACPI_MAX_IOAPICS];
static int count;
static const struct acpi_info *acpi;
static spinlock_t io_lock = SPINLOCK_INIT;

static uint32_t rd(struct ioapic *io, uint32_t reg)
{
    io->regs[IOREGSEL / 4] = reg;
    return io->regs[IOWIN / 4];
}

static void wr(struct ioapic *io, uint32_t reg, uint32_t value)
{
    io->regs[IOREGSEL / 4] = reg;
    io->regs[IOWIN / 4] = value;
}

static struct ioapic *find(uint32_t gsi)
{
    for (int i = 0; i < count; i++) {
        if (gsi >= ioapics[i].gsi_base && gsi < ioapics[i].gsi_base + ioapics[i].pins)
            return &ioapics[i];
    }
    return NULL;
}

static void add(uint8_t id, uint32_t addr, uint32_t gsi_base)
{
    if (count == ACPI_MAX_IOAPICS)
        return;
    struct ioapic *io = &ioapics[count];
    io->regs = vmm_ioremap(addr, 0x20);
    if (!io->regs) {
        printk("IOAPIC: cannot map %x\n", addr);
        return;
    }
    io->id = id;
    io->gsi_base = gsi_base;
    io->pins = ((rd(io, REG_VER) >> 16) & 0xFF) + 1;
    for (uint32_t i = 0; i < io->pins; i++) /* mask everything until a driver asks */
        wr(io, REG_REDIR(i), RED_MASKED);
    count++;
}

void ioapic_init(const struct acpi_info *a)
{
    acpi = a;
    count = 0;
    if (a->present) {
        for (int i = 0; i < a->ioapic_count; i++)
            add(a->ioapics[i].id, a->ioapics[i].addr, a->ioapics[i].gsi_base);
    } else {
        add(0, IOAPIC_DEFAULT_BASE, 0); /* no ACPI: assume the standard PC layout */
    }
    if (count == 0)
        kpanic("no usable I/O APIC");
}

int ioapic_route_gsi(uint32_t gsi, uint8_t vector, bool active_low, bool level)
{
    struct ioapic *io = find(gsi);
    if (!io)
        return -1;

    uint32_t pin = gsi - io->gsi_base;
    uint32_t low = vector | (level ? RED_LEVEL : 0) | (active_low ? RED_ACTIVE_LOW : 0);
    uint32_t high = apic_id() << 24;

    uint64_t f = spin_lock_irqsave(&io_lock);
    wr(io, REG_REDIR(pin) + 1, high);
    wr(io, REG_REDIR(pin), low); /* written last: this unmasks it */
    spin_unlock_irqrestore(&io_lock, f);
    return 0;
}

int ioapic_route_isa(uint8_t isa_irq, uint8_t vector)
{
    uint32_t gsi = isa_irq;
    bool active_low = false, level = false; /* ISA default: edge, active high */

    if (acpi && acpi->present) {
        for (int i = 0; i < acpi->iso_count; i++) {
            if (acpi->isos[i].irq != isa_irq)
                continue;
            gsi = acpi->isos[i].gsi;
            uint16_t pol = acpi->isos[i].flags & 3, trig = (acpi->isos[i].flags >> 2) & 3;
            active_low = (pol == 3);
            level = (trig == 3);
            break;
        }
    }
    return ioapic_route_gsi(gsi, vector, active_low, level);
}

uint32_t ioapic_isa_to_gsi(uint8_t isa_irq)
{
    if (acpi && acpi->present) {
        for (int i = 0; i < acpi->iso_count; i++) {
            if (acpi->isos[i].irq == isa_irq)
                return acpi->isos[i].gsi;
        }
    }
    return isa_irq;
}

void ioapic_mask_gsi(uint32_t gsi)
{
    struct ioapic *io = find(gsi);
    if (!io)
        return;
    uint64_t f = spin_lock_irqsave(&io_lock);
    wr(io, REG_REDIR(gsi - io->gsi_base), RED_MASKED);
    spin_unlock_irqrestore(&io_lock, f);
}

uint64_t ioapic_read_entry(uint32_t gsi)
{
    struct ioapic *io = find(gsi);
    if (!io)
        return ~0ULL;
    uint32_t pin = gsi - io->gsi_base;
    uint64_t f = spin_lock_irqsave(&io_lock);
    uint64_t v = ((uint64_t)rd(io, REG_REDIR(pin) + 1) << 32) | rd(io, REG_REDIR(pin));
    spin_unlock_irqrestore(&io_lock, f);
    return v;
}

int ioapic_count(void)
{
    return count;
}

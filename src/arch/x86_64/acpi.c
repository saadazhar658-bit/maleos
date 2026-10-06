#include "kernel/acpi.h"

#include "kernel/printk.h"
#include "kernel/string.h"
#include "mm/mm.h"
#include "mm/vmm.h"

/*
 * Minimal ACPI reader: just enough to find the I/O APICs and the ISA interrupt overrides
 * in the MADT. Tables can live anywhere in physical memory, so each is mapped on demand
 * through vmm_ioremap() (permanent but tiny).
 */

struct sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct rsdp {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_addr;
    /* ACPI 2.0+ */
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

static struct acpi_info info;

static bool checksum_ok(const void *p, size_t len)
{
    const uint8_t *b = p;
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++)
        sum += b[i];
    return sum == 0;
}

/* Map a whole table given its physical address. Returns NULL if it looks bogus. */
static const struct sdt_header *map_table(uint64_t phys)
{
    if (!phys)
        return NULL;
    const struct sdt_header *h = vmm_ioremap(phys, sizeof(struct sdt_header));
    if (!h || h->length < sizeof(*h) || h->length > (1u << 20))
        return NULL;
    h = vmm_ioremap(phys, h->length);
    if (!h || !checksum_ok(h, h->length))
        return NULL;
    return h;
}

static void parse_madt(const struct sdt_header *madt)
{
    const uint8_t *p = (const uint8_t *)madt + 44;
    const uint8_t *end = (const uint8_t *)madt + madt->length;
    info.lapic_addr = *(const uint32_t *)((const uint8_t *)madt + 36);

    while (p + 2 <= end) {
        uint8_t type = p[0], len = p[1];
        if (len < 2 || p + len > end)
            break;

        if (type == 1 && len >= 12 && info.ioapic_count < ACPI_MAX_IOAPICS) { /* I/O APIC */
            struct acpi_ioapic *io = &info.ioapics[info.ioapic_count++];
            io->id = p[2];
            io->addr = *(const uint32_t *)(p + 4);
            io->gsi_base = *(const uint32_t *)(p + 8);
        } else if (type == 2 && len >= 10 && info.iso_count < ACPI_MAX_ISOS) { /* override */
            struct acpi_iso *o = &info.isos[info.iso_count++];
            o->irq = p[3];
            o->gsi = *(const uint32_t *)(p + 4);
            o->flags = *(const uint16_t *)(p + 8);
        } else if (type == 5 && len >= 12) { /* local APIC address override */
            info.lapic_addr = (uint32_t) * (const uint64_t *)(p + 4);
        }
        p += len;
    }
    info.present = info.ioapic_count > 0;
}

void acpi_init(const struct boot_info *bi)
{
    memset(&info, 0, sizeof(info));
    if (!bi->has_rsdp)
        return;

    const struct rsdp *r = (const struct rsdp *)bi->rsdp;
    if (memcmp(r->signature, "RSD PTR ", 8) != 0 || !checksum_ok(r, 20)) {
        printk("ACPI: RSDP is invalid\n");
        return;
    }

    /* Prefer the 64-bit XSDT when the RSDP is revision 2+ and verifies. */
    bool use_xsdt = r->revision >= 2 && r->xsdt_addr && checksum_ok(r, sizeof(*r));
    const struct sdt_header *root = map_table(use_xsdt ? r->xsdt_addr : r->rsdt_addr);
    if (!root) {
        printk("ACPI: cannot read the root table\n");
        return;
    }

    size_t entry = use_xsdt ? 8 : 4;
    size_t n = (root->length - sizeof(*root)) / entry;
    const uint8_t *entries = (const uint8_t *)root + sizeof(*root);

    for (size_t i = 0; i < n; i++) {
        uint64_t phys =
            use_xsdt ? *(const uint64_t *)(entries + i * 8) : *(const uint32_t *)(entries + i * 4);
        const struct sdt_header *h = map_table(phys);
        if (h && memcmp(h->signature, "APIC", 4) == 0) {
            parse_madt(h);
            break;
        }
    }
}

const struct acpi_info *acpi_get(void)
{
    return &info;
}

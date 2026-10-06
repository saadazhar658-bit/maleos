#include "kernel/bootinfo.h"

#include "kernel/printk.h"
#include "kernel/string.h"
#include "mm/mm.h"

#define MB2_TAG_END 0
#define MB2_TAG_MODULE 3
#define MB2_TAG_MMAP 6
#define MB2_TAG_ACPI_OLD 14
#define MB2_TAG_ACPI_NEW 15

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

struct mb2_mmap_tag {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    /* entries follow */
};

struct mb2_mmap_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

void bootinfo_parse(uint64_t mbi_phys, struct boot_info *out)
{
    memset(out, 0, sizeof(*out));

    if (mbi_phys == 0 || (mbi_phys & 7) || mbi_phys >= BOOT_IDENTITY_LIMIT)
        kpanic("bad Multiboot2 info pointer %p", (void *)mbi_phys);

    const uint8_t *base = (const uint8_t *)mbi_phys; /* identity-mapped during early boot */
    uint32_t total = *(const uint32_t *)base;
    if (total < 16)
        kpanic("Multiboot2 info too small (%u bytes)", total);

    out->mbi_phys = mbi_phys;
    out->mbi_size = total;

    const uint8_t *p = base + 8;
    const uint8_t *end = base + total;

    while (p + sizeof(struct mb2_tag) <= end) {
        const struct mb2_tag *tag = (const struct mb2_tag *)p;
        if (tag->type == MB2_TAG_END)
            break;
        if (tag->size < sizeof(struct mb2_tag))
            kpanic("corrupt Multiboot2 tag (size %u)", tag->size);

        if (tag->type == MB2_TAG_MMAP) {
            const struct mb2_mmap_tag *mm = (const struct mb2_mmap_tag *)tag;
            if (mm->entry_size < sizeof(struct mb2_mmap_entry))
                kpanic("bad memory map entry size %u", mm->entry_size);

            const uint8_t *e = (const uint8_t *)tag + sizeof(*mm);
            const uint8_t *e_end = (const uint8_t *)tag + tag->size;
            for (; e + mm->entry_size <= e_end; e += mm->entry_size) {
                const struct mb2_mmap_entry *ent = (const struct mb2_mmap_entry *)e;
                if (out->region_count == BOOTINFO_MAX_REGIONS) {
                    printk("bootinfo: memory map truncated at %d regions\n", BOOTINFO_MAX_REGIONS);
                    break;
                }
                out->regions[out->region_count].base = ent->base;
                out->regions[out->region_count].length = ent->length;
                out->regions[out->region_count].type = ent->type;
                out->region_count++;
            }
        }

        if (tag->type == MB2_TAG_MODULE && tag->size >= 16) {
            const uint32_t *w = (const uint32_t *)tag;
            if (out->module_count < BOOTINFO_MAX_MODULES) {
                struct boot_module *m = &out->modules[out->module_count++];
                m->start = w[2];
                m->end = w[3];
                size_t n = tag->size - 16;
                if (n >= BOOTINFO_CMDLINE_MAX)
                    n = BOOTINFO_CMDLINE_MAX - 1;
                memcpy(m->name, (const uint8_t *)tag + 16, n);
                m->name[n] = 0;
            }
        }

        if ((tag->type == MB2_TAG_ACPI_OLD || tag->type == MB2_TAG_ACPI_NEW) &&
            tag->size >= 8 + 20) {
            size_t n = tag->size - 8;
            if (n > sizeof(out->rsdp))
                n = sizeof(out->rsdp);
            /* Prefer the newer (ACPI 2+) RSDP if both tags exist. */
            if (!out->has_rsdp || tag->type == MB2_TAG_ACPI_NEW) {
                memset(out->rsdp, 0, sizeof(out->rsdp));
                memcpy(out->rsdp, (const uint8_t *)tag + 8, n);
                out->has_rsdp = true;
            }
        }

        p += (tag->size + 7) & ~7u; /* tags are 8-byte aligned */
    }

    if (out->region_count == 0)
        kpanic("bootloader did not provide a memory map");
}

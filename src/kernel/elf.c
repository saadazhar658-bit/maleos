#include <stdint.h>

#include "kernel/errno.h"
#include "kernel/process.h"
#include "kernel/string.h"
#include "mm/mm.h"
#include "mm/uspace.h"
#include "mm/vmm.h"

/*
 * ELF64 loader for static executables. Everything in the file is untrusted: each header
 * field is range-checked before use, and a malformed image yields -ENOEXEC (or -ENOMEM)
 * with the address space left clean for the caller to destroy.
 */

#define EI_NIDENT 16
#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define MAX_PHDRS 16
#define MAX_SEGMENT_BYTES (16u << 20)

struct elf_header {
    uint8_t ident[EI_NIDENT];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf_phdr {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} __attribute__((packed));

/* Write into user pages regardless of their protection (text is mapped read-only). */
static int poke(struct process *p, uint64_t va, const uint8_t *src, uint64_t n)
{
    while (n) {
        uint64_t phys;
        if (!vmm_translate(p->pml4, va, &phys, NULL))
            return -ENOEXEC;
        uint64_t chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        if (chunk > n)
            chunk = n;
        memcpy(phys_to_virt(phys), src, chunk);
        src += chunk;
        va += chunk;
        n -= chunk;
    }
    return 0;
}

int elf_load(struct process *p, const void *image, size_t size, uint64_t *entry)
{
    const uint8_t *img = image;
    struct elf_header eh;
    if (size < sizeof(eh))
        return -ENOEXEC;
    memcpy(&eh, img, sizeof(eh));

    if (memcmp(eh.ident,
               "\x7f"
               "ELF",
               4) != 0 ||
        eh.ident[4] != 2 /* 64-bit */ || eh.ident[5] != 1 /* little endian */ || eh.ident[6] != 1 ||
        eh.type != 2 /* ET_EXEC */ || eh.machine != 62 /* x86-64 */ || eh.version != 1 ||
        eh.phentsize != sizeof(struct elf_phdr) || eh.phnum == 0 || eh.phnum > MAX_PHDRS)
        return -ENOEXEC;
    uint64_t table_bytes = (uint64_t)eh.phnum * sizeof(struct elf_phdr);
    if (eh.phoff > size || table_bytes > size - eh.phoff)
        return -ENOEXEC;

    uint64_t image_end = 0;
    bool entry_ok = false;
    int loaded = 0;

    for (int i = 0; i < eh.phnum; i++) {
        struct elf_phdr ph;
        memcpy(&ph, img + eh.phoff + (uint64_t)i * sizeof(ph), sizeof(ph));
        if (ph.type != PT_LOAD || ph.memsz == 0)
            continue; /* empty segments carry nothing */

        if ((ph.flags & PF_W) && (ph.flags & PF_X))
            return -ENOEXEC; /* W^X applies to user programs too */
        if (ph.filesz > ph.memsz || ph.memsz > MAX_SEGMENT_BYTES)
            return -ENOEXEC;
        if (ph.offset > size || ph.filesz > size - ph.offset)
            return -ENOEXEC;
        if (ph.vaddr < USER_IMAGE_MIN || ph.vaddr > USER_IMAGE_MAX ||
            ph.memsz > USER_IMAGE_MAX - ph.vaddr)
            return -ENOEXEC;

        uint64_t first = ALIGN_DOWN(ph.vaddr, PAGE_SIZE);
        uint64_t end = ALIGN_UP(ph.vaddr + ph.memsz, PAGE_SIZE);
        uint32_t prot = ((ph.flags & PF_W) ? VMM_WRITE : 0) | ((ph.flags & PF_X) ? VMM_EXEC : 0);

        /* Segments must not share pages (the linker script page-aligns them). */
        int rc = process_map(p, first, (end - first) / PAGE_SIZE, prot);
        if (rc == -EEXIST)
            return -ENOEXEC;
        if (rc < 0)
            return rc;
        if (poke(p, ph.vaddr, img + ph.offset, ph.filesz) < 0)
            return -ENOEXEC;

        if (end > image_end)
            image_end = end;
        if ((ph.flags & PF_X) && eh.entry >= ph.vaddr && eh.entry < ph.vaddr + ph.memsz)
            entry_ok = true;
        loaded++;
    }

    if (loaded == 0 || !entry_ok)
        return -ENOEXEC;

    p->brk_base = p->brk = image_end;
    p->brk_limit = image_end + (uint64_t)PROC_HEAP_PAGES * PAGE_SIZE;
    if (p->brk_limit > USER_IMAGE_MAX)
        p->brk_limit = USER_IMAGE_MAX;
    *entry = eh.entry;
    return 0;
}

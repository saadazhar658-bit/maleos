#include "drivers/ata_id.h"

#include "kernel/string.h"

/* IDENTIFY strings are stored as big-endian byte pairs inside little-endian words. */
static void copy_string(char *dst, const uint16_t *words, int nwords, size_t dst_size)
{
    size_t n = 0;
    for (int i = 0; i < nwords && n + 1 < dst_size; i++) {
        dst[n++] = (char)(words[i] >> 8);
        if (n + 1 < dst_size)
            dst[n++] = (char)(words[i] & 0xFF);
    }
    while (n > 0 && (dst[n - 1] == ' ' || dst[n - 1] == 0))
        n--;
    dst[n] = 0;
}

int ata_identify_parse(const uint16_t id[256], struct ata_ident *out)
{
    memset(out, 0, sizeof(*out));
    if (id[0] & 0x8000)
        return -1; /* bit 15 set: an ATAPI (packet) device, not a disk */

    copy_string(out->model, &id[27], 20, sizeof(out->model));

    out->lba = id[49] & (1u << 9);
    out->lba48 = id[83] & (1u << 10);
    if (!out->lba)
        return -1; /* CHS-only drives are not supported */

    out->sectors = (uint64_t)id[60] | ((uint64_t)id[61] << 16);
    if (out->lba48) {
        uint64_t s48 = (uint64_t)id[100] | ((uint64_t)id[101] << 16) | ((uint64_t)id[102] << 32) |
                       ((uint64_t)id[103] << 48);
        if (s48)
            out->sectors = s48;
    }
    if (out->sectors == 0)
        return -1;

    /* Words 106/117-118 describe logical sector sizes other than 512 bytes. */
    out->sector_size = 512;
    if ((id[106] & 0xC000) == 0x4000 && (id[106] & (1u << 12))) {
        uint32_t words = (uint32_t)id[117] | ((uint32_t)id[118] << 16);
        if (words)
            out->sector_size = words * 2;
    }
    return 0;
}

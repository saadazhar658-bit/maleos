#ifndef MALEOS_DRIVERS_ATA_ID_H
#define MALEOS_DRIVERS_ATA_ID_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/block.h"

struct ata_ident {
    uint64_t sectors;
    uint32_t sector_size;
    bool lba48;
    bool lba;
    char model[BLK_MODEL_MAX];
};

/* Decode the 256-word response to ATA IDENTIFY DEVICE. Returns 0, or -1 if unusable. */
int ata_identify_parse(const uint16_t id[256], struct ata_ident *out);

#endif

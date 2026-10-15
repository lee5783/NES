#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t PRG_reg;
    uint8_t CHR1_reg;
    uint8_t CHR2_reg;
    uint8_t *PRG_bank1;
    uint8_t *PRG_bank2;
    uint8_t *CHR_bank1;
    uint8_t *CHR_bank2;
    uint8_t CHR_mode;
    uint8_t PRG_mode;
    uint8_t reg;
    uint32_t PRG_clamp;
    uint32_t CHR_clamp;
    size_t cpu_cycle;
} MMC1_t;

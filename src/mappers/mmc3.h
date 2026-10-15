#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t *PRG_bank_ptrs[4];
    uint8_t *CHR_bank_ptrs[8];
    uint8_t PRG_mode;
    uint8_t CHR_inversion;
    uint8_t next_bank_data;
    uint8_t a12;
    uint8_t IRQ_latch;
    uint8_t IRQ_counter;
    uint8_t IRQ_cleared;
    uint8_t IRQ_enabled;
    uint8_t alt_clocking;
    uint8_t RAM_enabled;
    uint8_t RAM_protect;
    uint32_t PRG_clamp;
    uint32_t CHR_clamp;
    size_t cycles;
} MMC3_t;

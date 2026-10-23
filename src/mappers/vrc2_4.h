#pragma once

#include <stdint.h>

typedef struct {
    uint8_t PRG_reg[2];
    uint16_t CHR_reg[8];
    uint8_t PRG_swap;
    uint8_t IRQ_latch;
    uint8_t IRQ_counter;
    uint8_t IRQ_control;
    int16_t IRQ_prescaler;
    // load-time configuration
    uint8_t A0_lines;
    uint8_t A1_lines;
    uint8_t is_VRC2;
    uint8_t CHR_shift;
} VRC2_4_t;

#pragma once

#include <stdint.h>

typedef struct {
    uint8_t PRG_reg[3];
    uint8_t CHR_reg[8];
    uint8_t RAM_control;
    uint8_t IRQ_control;
    uint16_t IRQ_reload;
    uint16_t IRQ_counter;
} SS88006_t;

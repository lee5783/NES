#include <stdlib.h>
#include "mapper.h"
#include "vrc2_4.h"
#include "emulator.h"
#include "utils.h"
#include "cpu6502.h"

static uint8_t read_PRG(Mapper *mapper, uint16_t address);
static void write_PRG(Mapper *mapper, uint16_t address, uint8_t val);
static uint8_t read_CHR(Mapper *mapper, uint16_t address);
static void write_CHR(Mapper *mapper, uint16_t address, uint8_t val);
static void write_CHR_reg(Mapper *mapper, uint8_t bank, uint8_t high, uint8_t val);
static void write_IRQ(Mapper *mapper, uint8_t reg, uint8_t val);
static void clock_IRQ(Mapper *mapper);
static void update_PRG_ptrs(Mapper *mapper);

static const Mirroring mirroring_modes[4] = {VERTICAL, HORIZONTAL, ONE_SCREEN_LOWER, ONE_SCREEN_UPPER};

static void set_lines(VRC2_4_t *vrc, uint8_t submapper, uint8_t a0, uint8_t a1, uint8_t alt_a0, uint8_t alt_a1) {
    // iNES 1.0 (submapper 0) decodes both address line pairs, NES 2.0 picks one
    uint8_t first = submapper != 2;
    uint8_t second = submapper == 0 || submapper == 2;
    vrc->A0_lines = (first ? a0 : 0) | (second ? alt_a0 : 0);
    vrc->A1_lines = (first ? a1 : 0) | (second ? alt_a1 : 0);
}

int load_VRC2_4(Mapper *mapper) {
    VRC2_4_t *vrc = calloc(1, sizeof(VRC2_4_t));
    if (vrc == NULL)
        return -1;
    mapper->extension = vrc;
    mapper->read_PRG = read_PRG;
    mapper->write_PRG = write_PRG;
    mapper->read_CHR = read_CHR;
    mapper->write_CHR = write_CHR;

    switch (mapper->mapper_num) {
        case 21:
            // VRC4a (A1, A2), VRC4c (A6, A7)
            set_lines(vrc, mapper->submapper, BIT_1, BIT_2, BIT_6, BIT_7);
            break;
        case 22:
            // VRC2a (A1, A0), CHR bank number shifted right by one
            set_lines(vrc, 1, BIT_1, BIT_0, 0, 0);
            vrc->is_VRC2 = 1;
            vrc->CHR_shift = 1;
            break;
        case 23:
            // VRC2b / VRC4f (A0, A1), VRC4e (A2, A3)
            set_lines(vrc, mapper->submapper, BIT_0, BIT_1, BIT_2, BIT_3);
            vrc->is_VRC2 = mapper->submapper == 3;
            break;
        default:
            // VRC2c / VRC4b (A1, A0), VRC4d (A3, A2)
            set_lines(vrc, mapper->submapper, BIT_1, BIT_0, BIT_3, BIT_2);
            vrc->is_VRC2 = mapper->submapper == 3;
            break;
    }
    if (!vrc->is_VRC2)
        mapper->clock = clock_IRQ;

    update_PRG_ptrs(mapper);
    for (int i = 0; i < 8; i++)
        mapper->CHR_ptrs[i] = CHR_bank(mapper, 0x400, 0);
    return 0;
}

static void write_PRG(Mapper *mapper, uint16_t address, uint8_t val) {
    VRC2_4_t *vrc = mapper->extension;
    uint8_t reg = ((address & vrc->A0_lines) ? 1 : 0) | ((address & vrc->A1_lines) ? 2 : 0);

    switch (address & 0xf000) {
        case 0x8000:
            vrc->PRG_reg[0] = val & 0x1f;
            update_PRG_ptrs(mapper);
            break;
        case 0x9000:
            if (vrc->is_VRC2 || reg == 0) {
                if (mapper->mirroring != FOUR_SCREEN)
                    set_mirroring(mapper, mirroring_modes[val & (vrc->is_VRC2 ? 1 : 3)]);
            } else if (reg == 2) {
                vrc->PRG_swap = (val & BIT_1) != 0;
                update_PRG_ptrs(mapper);
            }
            break;
        case 0xa000:
            vrc->PRG_reg[1] = val & 0x1f;
            update_PRG_ptrs(mapper);
            break;
        case 0xb000:
        case 0xc000:
        case 0xd000:
        case 0xe000:
            write_CHR_reg(mapper, (uint8_t)(((address >> 12) - 0xb) * 2 + (reg >> 1)), reg & 1, val);
            break;
        case 0xf000:
            if (!vrc->is_VRC2)
                write_IRQ(mapper, reg, val);
            break;
        default:
            break;
    }
}

static void write_CHR_reg(Mapper *mapper, uint8_t bank, uint8_t high, uint8_t val) {
    VRC2_4_t *vrc = mapper->extension;
    uint16_t value = vrc->CHR_reg[bank];
    if (high)
        value = (uint16_t)((value & 0x0f) | ((val & (vrc->is_VRC2 ? 0x0f : 0x1f)) << 4));
    else
        value = (uint16_t)((value & 0x1f0) | (val & 0x0f));
    vrc->CHR_reg[bank] = value;
    mapper->CHR_ptrs[bank] = CHR_bank(mapper, 0x400, value >> vrc->CHR_shift);
}

static void write_IRQ(Mapper *mapper, uint8_t reg, uint8_t val) {
    VRC2_4_t *vrc = mapper->extension;
    switch (reg) {
        case 0:
            vrc->IRQ_latch = (uint8_t)((vrc->IRQ_latch & 0xf0) | (val & 0x0f));
            break;
        case 1:
            vrc->IRQ_latch = (uint8_t)((vrc->IRQ_latch & 0x0f) | ((val & 0x0f) << 4));
            break;
        case 2:
            vrc->IRQ_control = val & 0x07;
            vrc->IRQ_prescaler = 341;
            if (vrc->IRQ_control & BIT_1)
                vrc->IRQ_counter = vrc->IRQ_latch;
            interrupt_clear(&mapper->emulator->cpu, MAPPER_IRQ);
            break;
        default:
            // acknowledge: copy "enable after acknowledgement" into "enable"
            vrc->IRQ_control = (uint8_t)((vrc->IRQ_control & ~BIT_1) | ((vrc->IRQ_control & BIT_0) << 1));
            interrupt_clear(&mapper->emulator->cpu, MAPPER_IRQ);
            break;
    }
}

static void clock_IRQ(Mapper *mapper) {
    VRC2_4_t *vrc = mapper->extension;
    if (!(vrc->IRQ_control & BIT_1))
        return;
    if (!(vrc->IRQ_control & BIT_2)) {
        // scanline mode: 341 / 3 CPU cycles per clock
        vrc->IRQ_prescaler -= 3;
        if (vrc->IRQ_prescaler > 0)
            return;
        vrc->IRQ_prescaler += 341;
    }
    if (vrc->IRQ_counter == 0xff) {
        vrc->IRQ_counter = vrc->IRQ_latch;
        interrupt(&mapper->emulator->cpu, MAPPER_IRQ);
    } else {
        vrc->IRQ_counter++;
    }
}

static void update_PRG_ptrs(Mapper *mapper) {
    VRC2_4_t *vrc = mapper->extension;
    size_t banks = (size_t)mapper->PRG_banks * 2;
    uint8_t *second_last = PRG_bank(mapper, 0x2000, banks - 2);
    uint8_t *selected = PRG_bank(mapper, 0x2000, vrc->PRG_reg[0]);
    mapper->PRG_ptrs[0] = vrc->PRG_swap ? second_last : selected;
    mapper->PRG_ptrs[1] = PRG_bank(mapper, 0x2000, vrc->PRG_reg[1]);
    mapper->PRG_ptrs[2] = vrc->PRG_swap ? selected : second_last;
    mapper->PRG_ptrs[3] = PRG_bank(mapper, 0x2000, banks - 1);
}

static uint8_t read_PRG(Mapper *mapper, uint16_t address) {
    return mapper->PRG_ptrs[(address >> 13) & 3][address & 0x1fff];
}

static uint8_t read_CHR(Mapper *mapper, uint16_t address) {
    return mapper->CHR_ptrs[(address >> 10) & 7][address & 0x3ff];
}

static void write_CHR(Mapper *mapper, uint16_t address, uint8_t val) {
    if (!mapper->CHR_RAM_size) {
        LOG(DEBUG, "Attempted to write to CHR-ROM");
        return;
    }
    mapper->CHR_ptrs[(address >> 10) & 7][address & 0x3ff] = val;
}

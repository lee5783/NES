#include <stdlib.h>
#include "mapper.h"
#include "jaleco_ss88006.h"
#include "emulator.h"
#include "utils.h"
#include "cpu6502.h"

static uint8_t read_ROM(Mapper *mapper, uint16_t address);
static void write_ROM(Mapper *mapper, uint16_t address, uint8_t val);
static uint8_t read_PRG(Mapper *mapper, uint16_t address);
static void write_PRG(Mapper *mapper, uint16_t address, uint8_t val);
static uint8_t read_CHR(Mapper *mapper, uint16_t address);
static void write_CHR(Mapper *mapper, uint16_t address, uint8_t val);
static void write_PRG_reg(Mapper *mapper, uint8_t bank, uint8_t high, uint8_t val);
static void write_CHR_reg(Mapper *mapper, uint8_t bank, uint8_t high, uint8_t val);
static void write_IRQ(Mapper *mapper, uint16_t address, uint8_t val);
static void clock_IRQ(Mapper *mapper);

static const Mirroring mirroring_modes[4] = {HORIZONTAL, VERTICAL, ONE_SCREEN_LOWER, ONE_SCREEN_UPPER};

// $9002: bit 0 enables the PRG-RAM chip, bit 1 allows writes
#define RAM_ENABLE BIT_0
#define RAM_WRITE BIT_1

int load_SS88006(Mapper *mapper) {
    SS88006_t *ss = calloc(1, sizeof(SS88006_t));
    if (ss == NULL)
        return -1;
    mapper->extension = ss;
    mapper->read_ROM = read_ROM;
    mapper->write_ROM = write_ROM;
    mapper->read_PRG = read_PRG;
    mapper->write_PRG = write_PRG;
    mapper->read_CHR = read_CHR;
    mapper->write_CHR = write_CHR;
    mapper->clock = clock_IRQ;

    size_t banks = (size_t)mapper->PRG_banks * 2;
    for (int i = 0; i < 3; i++)
        mapper->PRG_ptrs[i] = PRG_bank(mapper, 0x2000, 0);
    mapper->PRG_ptrs[3] = PRG_bank(mapper, 0x2000, banks - 1);
    for (int i = 0; i < 8; i++)
        mapper->CHR_ptrs[i] = CHR_bank(mapper, 0x400, 0);
    return 0;
}

static uint8_t read_ROM(Mapper *mapper, uint16_t address) {
    if (address >= 0x8000)
        return mapper->read_PRG(mapper, address);
    SS88006_t *ss = mapper->extension;
    // expansion area, missing or disabled PRG-RAM: open bus
    if (address < 0x6000 || mapper->PRG_RAM == NULL || !(ss->RAM_control & RAM_ENABLE))
        return mapper->emulator->mem.bus;
    return mapper->PRG_RAM[(address - 0x6000) & mapper->PRG_RAM_clamp];
}

static void write_ROM(Mapper *mapper, uint16_t address, uint8_t val) {
    if (address >= 0x8000) {
        mapper->write_PRG(mapper, address, val);
        return;
    }
    SS88006_t *ss = mapper->extension;
    if (address < 0x6000 || mapper->PRG_RAM == NULL)
        return;
    if ((ss->RAM_control & (RAM_ENABLE | RAM_WRITE)) == (RAM_ENABLE | RAM_WRITE))
        mapper->PRG_RAM[(address - 0x6000) & mapper->PRG_RAM_clamp] = val;
}

static void write_PRG(Mapper *mapper, uint16_t address, uint8_t val) {
    SS88006_t *ss = mapper->extension;
    uint8_t reg = address & 3;

    switch (address & 0xf000) {
        case 0x8000:
            write_PRG_reg(mapper, reg >> 1, reg & 1, val);
            break;
        case 0x9000:
            if (reg < 2)
                write_PRG_reg(mapper, 2, reg, val);
            else if (reg == 2)
                ss->RAM_control = val & (RAM_ENABLE | RAM_WRITE);
            break;
        case 0xa000:
        case 0xb000:
        case 0xc000:
        case 0xd000:
            write_CHR_reg(mapper, (uint8_t)(((address >> 12) - 0xa) * 2 + (reg >> 1)), reg & 1, val);
            break;
        case 0xe000:
        case 0xf000:
            write_IRQ(mapper, address & 0xf003, val);
            break;
        default:
            break;
    }
}

static void write_PRG_reg(Mapper *mapper, uint8_t bank, uint8_t high, uint8_t val) {
    SS88006_t *ss = mapper->extension;
    // 6-bit bank number: low 4 bits, then high 2 bits
    if (high)
        ss->PRG_reg[bank] = (uint8_t)((ss->PRG_reg[bank] & 0x0f) | ((val & 0x03) << 4));
    else
        ss->PRG_reg[bank] = (uint8_t)((ss->PRG_reg[bank] & 0x30) | (val & 0x0f));
    mapper->PRG_ptrs[bank] = PRG_bank(mapper, 0x2000, ss->PRG_reg[bank]);
}

static void write_CHR_reg(Mapper *mapper, uint8_t bank, uint8_t high, uint8_t val) {
    SS88006_t *ss = mapper->extension;
    if (high)
        ss->CHR_reg[bank] = (uint8_t)((ss->CHR_reg[bank] & 0x0f) | ((val & 0x0f) << 4));
    else
        ss->CHR_reg[bank] = (uint8_t)((ss->CHR_reg[bank] & 0xf0) | (val & 0x0f));
    mapper->CHR_ptrs[bank] = CHR_bank(mapper, 0x400, ss->CHR_reg[bank]);
}

static void write_IRQ(Mapper *mapper, uint16_t address, uint8_t val) {
    SS88006_t *ss = mapper->extension;
    switch (address) {
        case 0xe000:
        case 0xe001:
        case 0xe002:
        case 0xe003: {
            // reload value nibbles, least significant first
            unsigned shift = (address & 3u) * 4u;
            unsigned reload = (ss->IRQ_reload & ~(0x0fu << shift)) | ((val & 0x0fu) << shift);
            ss->IRQ_reload = (uint16_t)reload;
            break;
        }
        case 0xf000:
            ss->IRQ_counter = ss->IRQ_reload;
            interrupt_clear(&mapper->emulator->cpu, MAPPER_IRQ);
            break;
        case 0xf001:
            ss->IRQ_control = val & 0x0f;
            interrupt_clear(&mapper->emulator->cpu, MAPPER_IRQ);
            break;
        case 0xf002:
            if (mapper->mirroring != FOUR_SCREEN)
                set_mirroring(mapper, mirroring_modes[val & 3]);
            break;
        default:
            // $F003: uPD7756 ADPCM speech control, not emulated
            break;
    }
}

static void clock_IRQ(Mapper *mapper) {
    SS88006_t *ss = mapper->extension;
    if (!(ss->IRQ_control & BIT_0))
        return;
    // only the low 4 / 8 / 12 / 16 bits count; a borrow out of them asserts the IRQ
    unsigned mask = 0xffff;
    if (ss->IRQ_control & BIT_3)
        mask = 0x000f;
    else if (ss->IRQ_control & BIT_2)
        mask = 0x00ff;
    else if (ss->IRQ_control & BIT_1)
        mask = 0x0fff;
    unsigned low = ss->IRQ_counter & mask;
    if (low == 0)
        interrupt(&mapper->emulator->cpu, MAPPER_IRQ);
    ss->IRQ_counter = (uint16_t)((ss->IRQ_counter & ~mask) | ((low - 1u) & mask));
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

#include "emulator.h"
#include "mapper.h"
#include "utils.h"

#define SUB0_DISABLED_READS 2

static void write_PRG(Mapper * mapper, uint16_t address, uint8_t value);
static uint8_t read_CHR(Mapper * mapper, uint16_t address);
static void write_CHR(Mapper * mapper, uint16_t address, uint8_t value);
static void reset(Mapper * mapper);

int load_mapper185(Mapper *mapper) {
    mapper->write_PRG = write_PRG;
    mapper->read_CHR = read_CHR;
    mapper->write_CHR = write_CHR;
    mapper->reset = reset;
    mapper->CHR_ptrs[0] = mapper->CHR_ROM;
    return 0;
}

static void reset(Mapper *mapper) {
    if (mapper->submapper != 0)
        return;
    mapper->CHR_regs[0] = 0;
    mapper->CHR_regs[1] = 0;
}

static void write_PRG(Mapper *mapper, uint16_t address, uint8_t value) {
    // 8k CHR bank selected determined by bit 0 - 1
    switch (mapper->submapper) {
        case 4: mapper->CHR_regs[0] = (value & 0x3) == 0; break;
        case 5: mapper->CHR_regs[0] = (value & 0x3) == 1; break;
        case 6: mapper->CHR_regs[0] = (value & 0x3) == 2; break;
        case 7: mapper->CHR_regs[0] = (value & 0x3) == 3; break;
        default: break;
    }
}

static uint8_t read_CHR(Mapper *mapper, uint16_t address) {
    if (!mapper->CHR_regs[0]) {
        if (mapper->submapper == 0
            && !(mapper->emulator->ppu.mask & (SHOW_BG | SHOW_SPRITE))
            && ++mapper->CHR_regs[1] >= SUB0_DISABLED_READS)
            mapper->CHR_regs[0] = 1;
        // CHR_ROM not enabled, return open bus
        return mapper->emulator->mem.bus;
    }
    return mapper->CHR_ROM[address];
}

static void write_CHR(Mapper *mapper, uint16_t address, uint8_t value) {
    LOG(DEBUG, "Attempted to write to CHR-ROM");
}

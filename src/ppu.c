#include <string.h>
#include <stdlib.h>
#include "ppu.h"
#include "emulator.h"
#include "utils.h"
#include "cpu6502.h"

static void update_NMI(PPU* ppu, uint8_t delay);
static uint8_t is_y_in_range(PPU* ppu, uint8_t y);
static void clear_oam(PPU* ppu);
static void evaluate_sprites(PPU* ppu);
static uint8_t get_bg_pixel(PPU *ppu);
static uint8_t get_sprite_pixel(PPU* ppu);
static uint8_t get_pixel(PPU* ppu);
static void inc_hori_v(PPU* ppu);
static void inc_vert_v(PPU* ppu);
static void inc_v(PPU* ppu);
static void inc_sec_oam_addr(PPU* ppu);
static uint8_t read_masked_oam(PPU* ppu);
uint32_t nes_palette[64];
static size_t screen_size;

void init_ppu(struct Emulator* emulator){
    to_pixel_format(nes_palette_raw, nes_palette, 64, ABGR8888);
    PPU* ppu = &emulator->ppu;
    memset(ppu, 0, sizeof(PPU));
#if NAMETABLE_MODE
    screen_size = sizeof(uint32_t) * VISIBLE_SCANLINES * VISIBLE_DOTS * 4;
#else
    screen_size = sizeof(uint32_t) * VISIBLE_SCANLINES * VISIBLE_DOTS;
#endif
    ppu->screen = malloc(screen_size);
    ppu->emulator = emulator;
    ppu->mapper = &emulator->mapper;
    ppu->pre_render = emulator->type == NTSC ? NTSC_SCANLINES_PER_FRAME : PAL_SCANLINES_PER_FRAME;
    reset_ppu(ppu);
}

void reset_ppu(PPU* ppu){
    ppu->t = ppu->x = 0;
    ppu->should_inc_vert_v = ppu->should_inc_hori_v = 0;
    ppu->dots = 1;
    ppu->scanlines = 261;
    ppu->w = 0;
    ppu->ctrl &= ~0xFC;
    ppu->mask = 0;
    ppu->status = 0;
    ppu->frames = 0;
    ppu->render_state_delay = 0;
    ppu->render_status = 0;
    ppu->supress_vblank = 0;
    memset(ppu->OAM_cache, 0, sizeof(ppu->OAM_cache));
    memset(ppu->OAM, 0, sizeof(ppu->OAM));
}

void exit_ppu(PPU* ppu) {
    if(ppu->screen != NULL) {
        free(ppu->screen);
    }
}

void set_latch(PPU* ppu, uint8_t value, uint8_t mask) {
    ppu->latch &= ~mask;
    ppu->latch |= value & mask;
    // if not all bits are driven, don't refresh decay if it is not complete yet
    if (mask == 0xff || ppu->latch_decay == 0)
        ppu->latch_decay = 60000;
}

void set_address(PPU* ppu, uint8_t address){
    if(!ppu->w){
        // first write
        ppu->t &= 0xff;
        ppu->t |= (address & 0x3f) << 8; // store only upto bit 14
        ppu->w = 1;
    }else{
        // second write
        ppu->t &= 0xff00;
        ppu->t |= address;
        ppu->v = ppu->t;
        ppu->mapper->set_bus(ppu->mapper, ppu->v);
        ppu->w = 0;
    }
}

void set_oam_address(PPU* ppu, uint8_t address){
    ppu->oam_address = address;
}

uint8_t read_oam(PPU* ppu){
    if (ppu->render_status && (ppu->scanlines < VISIBLE_SCANLINES || ppu->scanlines == ppu->pre_render)) {
        return ppu->sprite_eval_unit.buffer;
    }

    return read_masked_oam(ppu);
}

void write_oam(PPU* ppu, uint8_t value){
    if (ppu->render_status && (ppu->scanlines < VISIBLE_SCANLINES || ppu->scanlines == ppu->pre_render)) {
        // glitchy OAM increment bumping only the upper 6 bits
        // we'll mask with 0xfffc and not just 0xfc to keep upper byte intact for overflow detection
        ppu->oam_address += 4;
        ppu->oam_address &= 0xfffc;
        return;
    }
    ppu->OAM[ppu->oam_address++ & 0xff] = value;
}

void set_scroll(PPU* ppu, uint8_t coord){
    if(!ppu->w){
        // first write
        ppu->t &= ~X_SCROLL_BITS;
        ppu->t |= (coord >> 3) & X_SCROLL_BITS;
        ppu->x = coord & 0x7;
        ppu->w = 1;
    }else{
        // second write
        ppu->t &= ~Y_SCROLL_BITS;
        ppu->t |= ((coord & 0x7) << 12) | ((coord & 0xF8) << 2);
        ppu->w = 0;
    }
}

uint8_t read_ppu(PPU* ppu){
    uint8_t data = ppu->read_buffer;
    if (ppu->v >= 0x3F00)
        // read palette value immediately since it exists internally in PPU
        data = read_vram(ppu, ppu->v);

    // set-up buffer after 4 ppu clocks
    ppu->read_to_buffer = 4;
    return data;
}

void write_ppu(PPU* ppu, uint8_t value){
    // write to VRAM after 4 ppu clocks
    ppu->write_to_buffer = 4;
    ppu->write_val = value;
}

void dma(PPU* ppu, uint8_t address){
    schedule_dma(
        &ppu->emulator->cpu,
        DMA_OAM,
        address * 0x100,
        ppu->OAM,
        256,
        ppu->oam_address
    );
}

uint8_t read_vram(PPU* ppu, uint16_t address){
    address = address & 0x3fff;

    if(address < 0x2000) {
        return ppu->mapper->read_CHR(ppu->mapper, address);
    }

    if(address < 0x3F00){
        address = (address & 0xefff) - 0x2000;
        return ppu->V_RAM[ppu->mapper->name_table_map[address / 0x400] + (address & 0x3ff)];
    }

    if(address < 0x4000) {
        // palette RAM provide first 6 bits and remaining 2 bits are open bus
        uint8_t val = ppu->palette[(address - 0x3F00) % 0x20] & 0x3f | (ppu->latch & 0xc0);
        if (ppu->mask & 0x1)
            // greyscale mode; lower 4 bits = 0000
            return val & 0xf0;
        return val;
    }

    return 0;
}

void write_vram(PPU* ppu, uint16_t address, uint8_t value){
    address = address & 0x3fff;

    if(address < 0x2000)
        ppu->mapper->write_CHR(ppu->mapper, address, value);
    else if(address < 0x3F00){
        address = (address & 0xefff) - 0x2000;
        ppu->V_RAM[ppu->mapper->name_table_map[address / 0x400] + (address & 0x3ff)] = value;
    }

    else if(address < 0x4000) {
        address = (address - 0x3F00) % 0x20;
        if(address % 4 == 0) {
            ppu->palette[address] = value;
            ppu->palette[address ^ 0x10] = value;
        }
        else
            ppu->palette[address] = value;
    }

}

uint8_t read_status(PPU* ppu){
    uint8_t status = ppu->status;
    ppu->w = 0;
    ppu->status &= ~BIT_7; // reset v_blank
    if (ppu->scanlines == 241 && (ppu->dots == 1 || ppu->dots == 2 || ppu->dots == 3))
        ppu->supress_vblank = 1;
    else
        update_NMI(ppu, 0);
    return status;
}

void set_ctrl(PPU* ppu, uint8_t ctrl){
    ppu->ctrl = ctrl;
    update_NMI(ppu, 1);
    // set name table in temp address
    ppu->t &= ~0xc00;
    ppu->t |= (ctrl & BASE_NAMETABLE) << 10;
}

void set_mask(PPU* ppu, uint8_t mask) {
    uint8_t should_render = (mask & RENDER_BITS) > 0;
    if (should_render != ppu->render_status) {
        ppu->render_state_delay = 3;
    }
    ppu->mask = mask;
}

static void update_NMI(PPU* ppu, uint8_t delay) {
    if (delay) {
        ppu->nmi_delay = delay;
        return;
    }
    if(ppu->ctrl & BIT_7 && ppu->status & BIT_7 && !ppu->supress_vblank)
        interrupt(&ppu->emulator->cpu, NMI);
    else
        interrupt_clear(&ppu->emulator->cpu, NMI);
    ppu->supress_vblank = 0;
}

static void inc_hori_v(PPU* ppu) {
    // inc horizontal V
    if ((ppu->v & COARSE_X) == 31) {
        ppu->v &= ~COARSE_X;
        // switch horizontal nametable
        ppu->v ^= 0x400;
    } else ppu->v++;
}

static void inc_vert_v(PPU* ppu) {
    if((ppu->v & FINE_Y) != FINE_Y) {
        // increment coarse x
        ppu->v += 0x1000;
    }
    else{
        ppu->v &= ~FINE_Y;
        uint16_t coarse_y = (ppu->v & COARSE_Y) >> 5;
        if(coarse_y == 29){
            coarse_y = 0;
            // toggle bit 11 to switch vertical nametable
            ppu->v ^= 0x800;
        }
        else if(coarse_y == 31){
            // nametable not switched
            coarse_y = 0;
        }
        else{
            coarse_y++;
        }

        ppu->v = (ppu->v & ~COARSE_Y) | (coarse_y << 5);
    }
}

static void inc_v(PPU* ppu) {
    if (ppu->render_status &&  (ppu->scanlines < VISIBLE_SCANLINES || ppu->scanlines == ppu->pre_render))
        // glitchy increment of both vertical and horizontal
        ppu->should_inc_vert_v = ppu->should_inc_hori_v = 1;
    else
        ppu->should_inc_v = 1;
}

static void inc_sec_oam_addr(PPU* ppu) {
    if (ppu->sec_oam_address < 32)
        ppu->sec_oam_address++;
}

static uint8_t get_bg_pixel(PPU *ppu) {
    if (!(ppu->mask & SHOW_BG))
        return 0;

    if(!(ppu->mask & SHOW_BG_8) && ppu->dots <= 8)
        return 0;

    const PictureUnit* pu = &ppu->p_unit;
    const uint16_t bit_mux = 0x8000 >> ppu->x;
    const uint8_t pattern = (pu->pattern_LSB & bit_mux ? 1 : 0) | (pu->pattern_MSB & bit_mux ? 0b10 : 0);
    const uint8_t palette = (pu->attr_LSB & bit_mux ? 1 : 0) | (pu->attr_MSB & bit_mux ? 0b10 : 0);

    if (!pattern)
        return 0;

    return palette << 2 | pattern;
}

static uint8_t get_sprite_pixel(PPU* ppu) {
    uint8_t pattern = 0;
    SpriteUnit* priority_unit = NULL;
    for (int i = 0; i < 8; i++) {
        SpriteUnit* unit = &ppu->sprite_units[i];
        if (unit->halted || unit->x == 0) {
            if (!(unit->pattern_LSB | unit->pattern_MSB))
                continue;
            uint8_t pattern_bits = 0;
            if (unit->attr & FLIP_HORIZONTAL) {
                pattern_bits = unit->pattern_LSB & 1 | (unit->pattern_MSB & 1) << 1;
                unit->pattern_LSB >>= 1;
                unit->pattern_MSB >>= 1;
            } else {
                pattern_bits = unit->pattern_LSB >> 7 | (unit->pattern_MSB >> 7) << 1;
                unit->pattern_LSB <<= 1;
                unit->pattern_MSB <<= 1;
            }
            if (pattern_bits && priority_unit == NULL) {
                priority_unit = unit;
                pattern = pattern_bits;
            }
        } else if (--unit->x == 0) {
            unit->halted = 1;
        }
    }

    if (!(ppu->mask & SHOW_SPRITE))
        return 0;

    if (!(ppu->mask & SHOW_SPRITE_8) && ppu->dots <= 8)
        return 0;

    if (priority_unit == NULL)
        return 0;

    return 0x10 | (priority_unit->attr & 3) << 2 | pattern | priority_unit->attr & BIT_5 | (priority_unit->attr & BIT_2 ? BIT_6 : 0);
}

static uint8_t get_pixel(PPU* ppu) {
    uint8_t sprite_pixel = get_sprite_pixel(ppu);
    uint8_t bg_pixel = get_bg_pixel(ppu);

    switch ((bg_pixel > 0) << 1 | (sprite_pixel > 0)) {
        case 0b00:
            // this would be connected tied to EXT pins
            // for now let's tie them to ground
            return 0;
        case 0b01:
            // keep only the 5 relevant bits
            return sprite_pixel & 0x1f;
        case 0b10:
            return bg_pixel;
        case 0b11:
            if (sprite_pixel & BIT_6 && ppu->dots < 256)
                ppu->status |= SPRITE_0_HIT;

            if (sprite_pixel & BIT_5)
                return bg_pixel;
            return sprite_pixel & 0x1f;
        default:
            return 0;
    }
}

static uint8_t is_y_in_range(PPU* ppu, uint8_t y) {
    return (ppu->scanlines & 0xff) - y < 8 << (ppu->ctrl & LONG_SPRITE? 1 : 0);
}

static void clear_oam(PPU* ppu) {
    if (ppu->dots & 1)
        // writes happen on odd dots since we have the 1 dot ppu delay
        ppu->OAM_cache[ppu->sec_oam_address & 0x1f] = 0xff;
    else
        inc_sec_oam_addr(ppu);

    ppu->sprite_eval_unit.buffer = 0xff;
}

static uint8_t read_masked_oam(PPU* ppu) {
    // bits 2 - 4 of attr byte are unimplemented and should be zero
    return ppu->OAM[ppu->oam_address & 0xff] & ((ppu->oam_address & 0x03) == 0x02 ? 0xE3 : 0xFF);
}

static void evaluate_sprites(PPU* ppu) {
    // sprite evaluation
    SpriteEvalMachine* su = &ppu->sprite_eval_unit;

    // finish OAM clear and start evaluation
    if (ppu->dots == 65) {
        // OAM clear complete, move on to sprite evaluation
        su->state = READ_OAM_Y;
        su->has_sprite_zero = 0;
        su->has_overflown = 0;
    }

    switch (su->state) {
        case READ_OAM_Y:
            if (ppu->oam_address >= 256) {
                su->state = OAM_EOF;
                su->buffer = read_masked_oam(ppu);
                ppu->oam_address = ppu->oam_address + 4 & 0xfc;
                break;
            }
            su->buffer = read_masked_oam(ppu);
            su->state = CMP_OAM_Y;
            break;
        case CMP_OAM_Y:
            if (ppu->sec_oam_address < 32) {
                ppu->OAM_cache[ppu->sec_oam_address] = su->buffer;
                if (is_y_in_range(ppu, su->buffer)) {
                    ppu->sec_oam_address++;
                    su->state = READ_BYTE;
                    ppu->oam_address++;
                    // read the next 3 bytes into secondary OAM
                    su->remaining = 3;
                    // if this is the first sprite being processed, set sprite zero flag
                    if (ppu->dots == 66)
                        su->has_sprite_zero = 1;
                } else {
                    su->state = READ_OAM_Y;
                    // reset the lower 2 bits after the +4 to align OAM
                    // we use 0xfffc instead of 0xfc because OAM address can go beyond 0xff
                    // to allow detection of OAM overflow condition later
                    ppu->oam_address = ppu->oam_address + 4 & 0xfffc;
                }
            } else {
                // sprite overflow
                if (is_y_in_range(ppu, su->buffer)) {
                    ppu->status |= SPRITE_OVERFLOW;
                    su->has_overflown = 1;
                    su->state = READ_BYTE;
                    ppu->oam_address++;
                    // read the next 3 bytes
                    su->remaining = 3;
                } else {
                    su->state = READ_OAM_Y;
                    if ((ppu->oam_address & 0x03) == 3)
                        // glitchy +1 increment when misaligned OAM lands on the X value
                        ppu->oam_address++;
                    else
                        // glitchy increment of both inner (+1) and outer pointers (+4)
                        ppu->oam_address = ppu->oam_address + 5;
                }
                // sec OAM writes converted to reads if it is full
                // sec OAM address points to 0 when full
                su->buffer = ppu->OAM_cache[0];
            }
            break;
        case READ_BYTE:
            if (ppu->oam_address >= 256) {
                su->state = OAM_EOF;
                su->buffer = read_masked_oam(ppu);
                ppu->oam_address = ppu->oam_address + 4 & 0xfc;
                break;
            }

            su->buffer = read_masked_oam(ppu);
            ppu->oam_address++;
            if (su->remaining == 1 && !is_y_in_range(ppu, su->buffer))
                // if X position is not in range, reset lower 2 bits of OAM address
                ppu->oam_address &= 0xfffc;

            su->state = WRITE_BYTE;
            break;
        case WRITE_BYTE:
            if (ppu->sec_oam_address < 32)
                ppu->OAM_cache[ppu->sec_oam_address++] = su->buffer;
            else
                // sec OAM writes converted to reads from its last address
                su->buffer = ppu->OAM_cache[0];

            if (--su->remaining == 0) {
                if (su->has_overflown)
                    su->state = OAM_EOF;
                else
                    su->state = READ_OAM_Y;
            } else
                su->state = READ_BYTE;
            break;
        case OAM_EOF:
            if(ppu->dots & 1){
                su->buffer = read_masked_oam(ppu);
                // normal +4 increment with lower 2 bits masked
                ppu->oam_address = ppu->oam_address + 4 & 0xfc;
            } else {
                // sec OAM writes converted to reads from its last address
                su->buffer = ppu->OAM_cache[ppu->sec_oam_address & 0x1f];
            }
            break;
    }
}

static inline uint8_t fetch_vram(PPU* ppu, uint16_t address) {
    Mapper* mapper = ppu->mapper;
    address = address & 0x3fff;

    if (address < 0x2000) {
        if (mapper->read_CHR == mapper_default_read_CHR)
            return mapper->CHR_ROM[address];
        return mapper->read_CHR(mapper, address);
    }

    if (address < 0x3F00) {
        address = (address & 0xefff) - 0x2000;
        return ppu->V_RAM[mapper->name_table_map[address / 0x400] + (address & 0x3ff)];
    }

    return read_vram(ppu, address);
}

static inline void notify_bus(PPU* ppu, uint16_t address) {
    Mapper* mapper = ppu->mapper;
    if (mapper->set_bus != mapper_default_set_bus)
        mapper->set_bus(mapper, address);
}

static void finish_fetch(PPU* ppu, PPUPhase phase) {
    if (!(phase & 1)) {
        ppu->p_unit.has_set_addr = 1;
        notify_bus(ppu, ppu->bus);
    } else
        ppu->p_unit.has_set_addr = 0;

    // glitchy increments on $2007 read
    if (ppu->should_inc_hori_v) {
        inc_hori_v(ppu);
        ppu->should_inc_hori_v = 0;
    }
    if (ppu->should_inc_vert_v) {
        inc_vert_v(ppu);
        ppu->should_inc_vert_v = 0;
    }
}

static void fetch_background(PPU* ppu, PPUPhase phase) {
    PictureUnit* pu = &ppu->p_unit;

    pu->pattern_LSB = pu->pattern_LSB << 1 | 1;
    pu->pattern_MSB = pu->pattern_MSB << 1 | 1;
    pu->attr_LSB  = pu->attr_LSB << 1 | 1;
    pu->attr_MSB = pu->attr_MSB << 1 | 1;

    if (ppu->dots > 320) {
        ppu->sprite_eval_unit.buffer = ppu->OAM_cache[ppu->sec_oam_address & 0x1f];
    }

    switch (phase) {
        case NT_ADDR:
            ppu->bus = 0x2000 | ppu->v & 0xFFF;
            break;
        case NT_READ:
            ppu->bus = ppu->bus & 0xff00 | fetch_vram(ppu, ppu->bus);
            pu->NT = ppu->bus & 0xff;
            break;
        case AT_ADDR:
            ppu->bus = 0x23C0 | ppu->v & 0x0C00 | ppu->v >> 4 & 0x38 | ppu->v >> 2 & 0x07;
            break;
        case AT_READ:
            ppu->bus = ppu->bus & 0xff00 | fetch_vram(ppu, ppu->bus);
            pu->AT = ppu->bus & 0xFF;
            pu->AT >>= ppu->v >> 4 & 4 | ppu->v & 2;
            break;
        case BG_LSB_ADDR:
            ppu->bus = pu->NT << 4 | ppu->v >> 12 & 0x7 | (ppu->ctrl & BG_TABLE) << 8;
            ppu->last_addr = ppu->bus;
            break;
        case BG_LSB_READ:
            ppu->bus = ppu->bus & 0xff00 | fetch_vram(ppu, ppu->bus);
            pu->BG_LSB = ppu->bus & 0xff;
            break;
        case BG_MSB_ADDR:
            ppu->bus = ppu->last_addr + 8;
            break;
        case BG_MSB_READ:
            ppu->bus = ppu->bus & 0xff00 | fetch_vram(ppu, ppu->bus);
            pu->BG_MSB = ppu->bus & 0xff;

            if (ppu->dots == 256) {
                ppu->should_inc_vert_v = 0;
                inc_vert_v(ppu);
            }

            ppu->should_inc_hori_v = 0;
            inc_hori_v(ppu);

            pu->pattern_LSB = pu->pattern_LSB & 0xff00 | pu->BG_LSB;
            pu->pattern_MSB = pu->pattern_MSB & 0xff00 | pu->BG_MSB;
            pu->attr_LSB = pu->attr_LSB & 0xff00 | (pu->AT & 0b01 ? 0xff : 0) ;
            pu->attr_MSB = pu->attr_MSB & 0xff00 | (pu->AT & 0b10 ? 0xff : 0) ;
            break;
    }

    finish_fetch(ppu, phase);
}

static void fetch_frame(PPU* ppu) {
    PPUPhase phase = (ppu->dots - 1) & 7;
    PictureUnit* pu = &ppu->p_unit;

    if (ppu->dots <= 256 || (ppu->dots > 320 && ppu->dots < 337)) {
        fetch_background(ppu, phase);
        return;
    }

    uint8_t sprite_prefetch = ppu->dots > 256 && ppu->dots <= 320;
    uint8_t pre_render = ppu->dots >= 337;

    // clock bg output shift registers
    if (ppu->dots <= 256 || (ppu->dots > 320 && ppu->dots < 337)) {
        pu->pattern_LSB = pu->pattern_LSB << 1 | 1;
        pu->pattern_MSB = pu->pattern_MSB << 1 | 1;
        pu->attr_LSB  = pu->attr_LSB << 1 | 1;
        pu->attr_MSB = pu->attr_MSB << 1 | 1;
    }

    if (sprite_prefetch) {
        ppu->oam_address = 0;
    }
    if (ppu->dots > 320) {
        ppu->sprite_eval_unit.buffer = ppu->OAM_cache[ppu->sec_oam_address & 0x1f];
    }

    switch (phase) {
        case NT_ADDR: // 0
            // load NT address
            ppu->bus = 0x2000 | ppu->v & 0xFFF;
            if (ppu->dots == 257) {
                ppu->v &= ~HORIZONTAL_BITS;
                ppu->v |= ppu->t & HORIZONTAL_BITS;
            }

            if (sprite_prefetch) {
                ppu->sprite_buffer.y = ppu->sprite_eval_unit.buffer = ppu->OAM_cache[ppu->sec_oam_address & 0x1f];
                inc_sec_oam_addr(ppu);
            }
            break;
        case NT_READ: // 1
            // load NT byte;
            ppu->bus = ppu->bus & 0xff00 | fetch_vram(ppu, ppu->bus);
            pu->NT = ppu->bus & 0xff;
            if (sprite_prefetch) {
                ppu->sprite_buffer.tile = ppu->sprite_eval_unit.buffer = ppu->OAM_cache[ppu->sec_oam_address & 0x1f];
                inc_sec_oam_addr(ppu);
            }
            break;
        case AT_ADDR: // 2
            if (sprite_prefetch || pre_render)
                // load NT address
                ppu->bus = 0x2000 | ppu->v & 0xFFF;
            else
                // load AT address
                ppu->bus = 0x23C0 | ppu->v & 0x0C00 | ppu->v >> 4 & 0x38 | ppu->v >> 2 & 0x07;
            if (sprite_prefetch) {
                ppu->sprite_buffer.attr = ppu->sprite_eval_unit.buffer = ppu->OAM_cache[ppu->sec_oam_address & 0x1f];
                inc_sec_oam_addr(ppu);
                SpriteUnit* unit = ppu->sprite_units + ((ppu->dots - 257) >> 3);
                // use unimplemented BIT 2 to mark spite zero
                unit->attr = ppu->sprite_buffer.attr | (ppu->dots == 259 && ppu->sprite_eval_unit.has_sprite_zero ? BIT_2 : 0);
            }
            break;
        case AT_READ: // 3
            // load AT/NT byte
            ppu->bus = ppu->bus & 0xff00 | fetch_vram(ppu, ppu->bus);
            pu->AT = ppu->bus & 0xFF;
            pu->AT >>= ppu->v >> 4 & 4 | ppu->v & 2;
            if (sprite_prefetch) {
                ppu->sprite_buffer.x = ppu->sprite_eval_unit.buffer = ppu->OAM_cache[ppu->sec_oam_address & 0x1f];
                SpriteUnit* unit = ppu->sprite_units + ((ppu->dots - 257) >> 3);
                unit->x = ppu->sprite_buffer.x;
                unit->halted = 1;
            }
            break;
        case BG_LSB_ADDR: // 4
            if (sprite_prefetch) {
                // load sprite LSB addr
                uint8_t offset = (ppu->scanlines & 0xff) - ppu->sprite_buffer.y;

                if (ppu->ctrl & LONG_SPRITE) {
                    // 8x16 sprite
                    if (ppu->sprite_buffer.attr & FLIP_VERTICAL)
                        offset ^= 15;
                    ppu->bus = (((ppu->sprite_buffer.tile & 0x1) << 12) | ((ppu->sprite_buffer.tile & ~0x1) << 4)) + ((offset & 0x8) << 1) + (offset & 0x7);
                } else {
                    // 8x8 sprite
                    if (ppu->sprite_buffer.attr & FLIP_VERTICAL)
                        offset ^= 7;
                    ppu->bus = ((ppu->ctrl & SPRITE_TABLE ? 0x1000: 0) | (ppu->sprite_buffer.tile << 4)) + (offset & 0x7);
                }
            } else {
                // load BG LSB addr
                ppu->bus = pu->NT << 4 | ppu->v >> 12 & 0x7 | (ppu->ctrl & BG_TABLE) << 8;
            }
            // address will be reused later so save it
            // the next read will corrupt lower byte
            ppu->last_addr = ppu->bus;
            break;
        case BG_LSB_READ: // 5
            ppu->bus = ppu->bus & 0xff00 | fetch_vram(ppu, ppu->bus);
            if (sprite_prefetch) {
                // load sprite LSB byte
                uint8_t sprite_index = (ppu->dots - 257) >> 3;
                SpriteUnit* unit = ppu->sprite_units + sprite_index;
                unit->pattern_LSB = ppu->bus & 0xff;
            }else {
                // load BG LSB byte
                pu->BG_LSB = ppu->bus & 0xff;
            }
            break;
        case BG_MSB_ADDR: // 6
            // load BG MSB / sprite MSB addr
            ppu->bus = ppu->last_addr + 8;
            break;
        case BG_MSB_READ: // 7
            ppu->bus = ppu->bus & 0xff00 | fetch_vram(ppu, ppu->bus);
            if (sprite_prefetch) {
                // load sprite MSB byte
                uint8_t sprite_index = (ppu->dots - 257) >> 3;
                SpriteUnit* unit = ppu->sprite_units + sprite_index;
                unit->pattern_MSB = ppu->bus & 0xff;
                inc_sec_oam_addr(ppu);

                if (!is_y_in_range(ppu, ppu->sprite_buffer.y)) {
                    // sprite is not in range
                    unit->pattern_MSB = 0;
                    unit->pattern_LSB = 0;
                }
            }else {
                // load BG MSB byte
                pu->BG_MSB = ppu->bus & 0xff;
            }

            if (ppu->dots == 256) {
                // prevent double vertical inc on $2007 read
                ppu->should_inc_vert_v = 0;
                inc_vert_v(ppu);
            }

            if (ppu->dots <= 256 || ppu->dots > 320) {
                // prevent double horizontal in on $2007 read
                ppu->should_inc_hori_v = 0;
                inc_hori_v(ppu);
            }

            // shift relevant data for rendering
            pu->pattern_LSB = pu->pattern_LSB & 0xff00 | pu->BG_LSB;
            pu->pattern_MSB = pu->pattern_MSB & 0xff00 | pu->BG_MSB;
            pu->attr_LSB = pu->attr_LSB & 0xff00 | (pu->AT & 0b01 ? 0xff : 0) ;
            pu->attr_MSB = pu->attr_MSB & 0xff00 | (pu->AT & 0b10 ? 0xff : 0) ;

            break;
        default:
            // we should never get here
            break;
    }

    finish_fetch(ppu, phase);
}

static void execute_dot(PPU* ppu) {
    // update render status
    if (ppu->render_state_delay && --ppu->render_state_delay == 0) {
        ppu->render_status = (ppu->mask & RENDER_BITS) > 0;
        if (!ppu->render_status && (ppu->scanlines < VISIBLE_SCANLINES || ppu->scanlines == ppu->pre_render)) {
            ppu->corrupt_oam_row = ppu->sec_oam_address & 0x1f;
            if (ppu->dots < 64 && ppu->dots <= 256)
                // round up to nearest multiple of 4
                ppu->corrupt_oam_row = (ppu->corrupt_oam_row + 3) & 0xfc;
        }
    }

    if (ppu->should_inc_v) {
        ppu->v += ((ppu->ctrl & BIT_2) ? 32 : 1);
        ppu->should_inc_v = 0;
        notify_bus(ppu, ppu->v);
    }

    const size_t dots = ppu->dots;
    const size_t scanlines = ppu->scanlines;

    if (scanlines < VISIBLE_SCANLINES || scanlines == ppu->pre_render) {
        uint8_t units_clocked = 0;
        if (ppu->corrupt_oam_row && ppu->render_status) {
            ppu->corrupt_oam_row &= 0x1f;
            memcpy(ppu->OAM + (ppu->corrupt_oam_row << 3), ppu->OAM, 8);
            ppu->OAM_cache[ppu->corrupt_oam_row] = ppu->OAM_cache[0];
            ppu->corrupt_oam_row = 0;
        }
        if (dots == 0) {
            if (scanlines <= VISIBLE_SCANLINES && ppu->render_status) {
                ppu->bus = ppu->p_unit.NT << 4 | ppu->v >> 12 & 0x7 | (ppu->ctrl & BG_TABLE) << 8;
                notify_bus(ppu, ppu->bus);
            }
        } else {
            // dots 1 - 256, scanline 0 - 239 (render region)
            if (scanlines < VISIBLE_SCANLINES && dots <= VISIBLE_DOTS) {
                uint8_t palette_index = 0;
                if (ppu->render_status) {
                    if (dots <= 64)
                        clear_oam(ppu);
                    else
                        evaluate_sprites(ppu);
                    palette_index = get_pixel(ppu);
                    units_clocked = 1;
                } else if ((ppu->v & 0x3fff) > 0x3f00) {
                    // if the low 14 bits of v fall within 0x3f00-0x3fff, use that value as backdrop address
                    palette_index = ppu->v & 0x1f;
                }
                // only consider 6-bits from palette RAM since upper 2 bits are open bus
                uint8_t color = ppu->palette[palette_index] & (ppu->mask & 0x1 ? 0x30 : 0x3f);
                // output pixel to output buffer
                ppu->screen[scanlines * 256 + dots - 1] = nes_palette[color];
            }
            if (ppu->render_status)
                // tile and attr pre-fetch
                fetch_frame(ppu);
        }

        if (!units_clocked) {
            for (int i = 0; i < 8; i++) {
                SpriteUnit* unit = ppu->sprite_units + i;
                if (!unit->halted && unit->x) {
                    if (--unit->x == 0) unit->halted = 1;
                }
            }
        }

        if (ppu->shift_start_delay && --ppu->shift_start_delay == 0) {
            for (int i = 0; i < 8; i++) {
               ppu->sprite_units[i].halted = 0;
            }
        }

        if (ppu->render_status) {
            if (dots == 340) {
                // tell sprite shifter to go
                ppu->shift_start_delay = 1;
                // reset secondary OAM address
                ppu->sec_oam_address = 0;
            } else if (dots == 256 || dots == 64) {
                // reset secondary OAM address
                ppu->sec_oam_address = 0;
            }

        }
    }

    if (ppu->read_to_buffer) {
        switch (ppu->read_to_buffer) {
            case 3:
                // place read_address on bus if fetch unit has not already
                if (!ppu->p_unit.has_set_addr)
                    // read underlying nametable mirrors if v is pointing to palette address
                    // in that case 0x3f00 - 0x3fff maps to 0x2f00 - 0x2fff
                    ppu->bus = ppu->v >= 0x3f00 ? ppu->v & 0xefff : ppu->v;
                break;
            case 2:
                // increment v on next cycle
                inc_v(ppu);
                break;
            case 1:
                // read to buffer
                ppu->read_buffer = read_vram(ppu, ppu->bus);
                break;
            default:
                break;
        }
        ppu->read_to_buffer--;
    }

    if (ppu->write_to_buffer) {
        switch (ppu->write_to_buffer) {
            case 3:
                // place write_address on bus
                if (!ppu->p_unit.has_set_addr)
                    ppu->bus = ppu->v;
                break;
            case 2:
                // increment v on next cycle
                inc_v(ppu);
                break;
            case 1:
                // write value to VRAM
                write_vram(ppu, ppu->bus, ppu->write_val);
                ppu->bus = ppu->bus & 0xff00 | ppu->write_val;
                break;
            default:
                break;
        }
        ppu->write_to_buffer--;
    }
    ppu->p_unit.has_set_addr = 0;

    if (scanlines >= VISIBLE_SCANLINES) {
        if (scanlines == 240 && dots == 0 && ppu->render_status) {
            ppu->bus = ppu->p_unit.NT << 4 | ppu->v >> 12 & 0x7 | (ppu->ctrl & BG_TABLE) << 8;
            notify_bus(ppu, ppu->bus);
        }

        if(scanlines == 241 && dots == 1) {
            // set v-blank
            if (!ppu->supress_vblank)
                ppu->status |= V_BLANK;
            ppu->supress_vblank = 0;
            update_NMI(ppu, 3);
        }

        if (scanlines == ppu->pre_render) {
            // pre-render scanline 262/312
            if (dots == 0) {
                ppu->status &= ~(SPRITE_0_HIT | SPRITE_OVERFLOW);
            }
            else if(dots == 1){
                // reset v-blank and sprite zero hit
                ppu->status &= ~V_BLANK;
                update_NMI(ppu, 0);
            }
            else if(dots >= 280 && dots <= 304 && ppu->render_status){
                ppu->v &= ~VERTICAL_BITS;
                ppu->v |= ppu->t & VERTICAL_BITS;
            }

            if(dots >= 340) {
                // inform emulator to render contents of ppu on first dot
                ppu->render = 1;
                ppu->frames++;
            }
        }
    }

    //update NMI delay
    if (ppu->nmi_delay && --ppu->nmi_delay == 0) {
        update_NMI(ppu, 0);
    }

    // open bus decay delay
    if (ppu->latch_decay && --ppu->latch_decay == 0) {
        ppu->latch = 0;
    }

    // increment dots and scanlines
    if(++ppu->dots >= DOTS_PER_SCANLINE) {
        if (ppu->scanlines++ >= ppu->pre_render) {
            // skip one cycle on odd frames if rendering is enabled for NTSC
            if (ppu->frames & 1 && ppu->render_status && ppu->emulator->type == NTSC)
                ppu->dots = 1;
            else
                ppu->dots = 0;
            ppu->scanlines = 0;
        } else {
            ppu->dots = 0;
        }
    }
}

static void execute_render_dot(PPU* ppu, size_t dots) {
    if (dots <= 64)
        clear_oam(ppu);
    else
        evaluate_sprites(ppu);
    uint8_t color = ppu->palette[get_pixel(ppu)] & (ppu->mask & 0x1 ? 0x30 : 0x3f);
    ppu->screen[ppu->scanlines * 256 + dots - 1] = nes_palette[color];

    fetch_background(ppu, (dots - 1) & 7);
    ppu->p_unit.has_set_addr = 0;

    if (dots == 256 || dots == 64)
        ppu->sec_oam_address = 0;

    if (ppu->latch_decay && --ppu->latch_decay == 0) {
        ppu->latch = 0;
    }
    ppu->dots = dots + 1;
}

static uint8_t is_quiet(const PPU* ppu) {
    return !(ppu->render_state_delay | ppu->should_inc_v | ppu->corrupt_oam_row | ppu->read_to_buffer
        | ppu->write_to_buffer | ppu->nmi_delay | ppu->shift_start_delay
        | ppu->should_inc_hori_v | ppu->should_inc_vert_v);
}

static uint8_t can_render_span(const PPU* ppu) {
    return ppu->dots - 1 < VISIBLE_DOTS && ppu->scanlines < VISIBLE_SCANLINES && ppu->render_status && is_quiet(ppu);
}

static void count_down_latch(PPU* ppu, size_t count) {
    if (ppu->latch_decay) {
        if (ppu->latch_decay <= count) {
            ppu->latch_decay = 0;
            ppu->latch = 0;
        } else
            ppu->latch_decay -= (uint16_t)count;
    }
}

static inline uint8_t masked_oam(const uint8_t* OAM, uint16_t address) {
    return OAM[address & 0xff] & ((address & 0x03) == 0x02 ? 0xE3 : 0xFF);
}

static inline uint8_t in_sprite_range(uint8_t line, uint8_t y, unsigned height) {
    return (unsigned)(line - y) < height;
}

static void evaluate_span(PPU* ppu, size_t first, size_t last) {
    size_t dots = first;
    uint8_t sec = ppu->sec_oam_address;
    for (; dots <= last && dots <= 64; dots++) {
        if (dots & 1)
            ppu->OAM_cache[sec & 0x1f] = 0xff;
        else if (sec < 32)
            sec++;
        ppu->sprite_eval_unit.buffer = 0xff;
        if (dots == 64)
            sec = 0;
    }
    if (dots > last) {
        ppu->sec_oam_address = sec;
        return;
    }

    SpriteEvalMachine* su = &ppu->sprite_eval_unit;
    const uint8_t* OAM = ppu->OAM;
    uint8_t* cache = ppu->OAM_cache;
    const uint8_t line = ppu->scanlines & 0xff;
    const unsigned height = 8u << (ppu->ctrl & LONG_SPRITE ? 1 : 0);
    SpriteEvalState state = su->state;
    uint8_t buffer = su->buffer;
    uint8_t remaining = su->remaining;
    uint8_t has_sprite_zero = su->has_sprite_zero;
    uint8_t has_overflown = su->has_overflown;
    uint16_t oam = ppu->oam_address;
    uint8_t status = ppu->status;

    for (; dots <= last; dots++) {
        if (dots == 65) {
            state = READ_OAM_Y;
            has_sprite_zero = 0;
            has_overflown = 0;
        }

        switch (state) {
            case READ_OAM_Y:
                if (oam >= 256) {
                    state = OAM_EOF;
                    buffer = masked_oam(OAM, oam);
                    oam = oam + 4 & 0xfc;
                    break;
                }
                buffer = masked_oam(OAM, oam);
                state = CMP_OAM_Y;
                break;
            case CMP_OAM_Y:
                if (sec < 32) {
                    cache[sec] = buffer;
                    if (in_sprite_range(line, buffer, height)) {
                        sec++;
                        state = READ_BYTE;
                        oam++;
                        remaining = 3;
                        if (dots == 66)
                            has_sprite_zero = 1;
                    } else {
                        state = READ_OAM_Y;
                        oam = oam + 4 & 0xfffc;
                    }
                } else {
                    if (in_sprite_range(line, buffer, height)) {
                        status |= SPRITE_OVERFLOW;
                        has_overflown = 1;
                        state = READ_BYTE;
                        oam++;
                        remaining = 3;
                    } else {
                        state = READ_OAM_Y;
                        if ((oam & 0x03) == 3)
                            oam++;
                        else
                            oam = oam + 5;
                    }
                    buffer = cache[0];
                }
                break;
            case READ_BYTE:
                if (oam >= 256) {
                    state = OAM_EOF;
                    buffer = masked_oam(OAM, oam);
                    oam = oam + 4 & 0xfc;
                    break;
                }
                buffer = masked_oam(OAM, oam);
                oam++;
                if (remaining == 1 && !in_sprite_range(line, buffer, height))
                    oam &= 0xfffc;
                state = WRITE_BYTE;
                break;
            case WRITE_BYTE:
                if (sec < 32)
                    cache[sec++] = buffer;
                else
                    buffer = cache[0];

                if (--remaining == 0) {
                    if (has_overflown)
                        state = OAM_EOF;
                    else
                        state = READ_OAM_Y;
                } else
                    state = READ_BYTE;
                break;
            case OAM_EOF:
                if (dots & 1) {
                    buffer = masked_oam(OAM, oam);
                    oam = oam + 4 & 0xfc;
                } else {
                    buffer = cache[sec & 0x1f];
                }
                break;
        }

        if (dots == 256)
            sec = 0;
    }

    su->state = state;
    su->buffer = buffer;
    su->remaining = remaining;
    su->has_sprite_zero = has_sprite_zero;
    su->has_overflown = has_overflown;
    ppu->oam_address = oam;
    ppu->sec_oam_address = sec;
    ppu->status = status;
}

static void shift_sprite_span(PPU* ppu, uint8_t* line, size_t count) {
    memset(line, 0, count);
    for (int i = 7; i >= 0; i--) {
        SpriteUnit* unit = &ppu->sprite_units[i];
        size_t k = 0;
        if (!unit->halted && unit->x) {
            if (unit->x >= count) {
                unit->x -= (uint8_t)count;
                if (unit->x == 0)
                    unit->halted = 1;
                continue;
            }
            k = unit->x;
            unit->x = 0;
            unit->halted = 1;
        }
        const uint8_t attr = unit->attr;
        const uint8_t pixel = 0x10 | (attr & 3) << 2 | attr & BIT_5 | (attr & BIT_2 ? BIT_6 : 0);
        uint8_t lsb = unit->pattern_LSB;
        uint8_t msb = unit->pattern_MSB;
        if (attr & FLIP_HORIZONTAL) {
            for (; k < count && (lsb | msb); k++) {
                uint8_t pattern_bits = lsb & 1 | (msb & 1) << 1;
                lsb >>= 1;
                msb >>= 1;
                if (pattern_bits)
                    line[k] = pixel | pattern_bits;
            }
        } else {
            for (; k < count && (lsb | msb); k++) {
                uint8_t pattern_bits = lsb >> 7 | (msb >> 7) << 1;
                lsb <<= 1;
                msb <<= 1;
                if (pattern_bits)
                    line[k] = pixel | pattern_bits;
            }
        }
        unit->pattern_LSB = lsb;
        unit->pattern_MSB = msb;
    }
}

static void render_span(PPU* ppu, size_t count) {
    const size_t first = ppu->dots;
    const size_t last = first + count - 1;
    uint8_t sprites[VISIBLE_DOTS];

    evaluate_span(ppu, first, last);
    shift_sprite_span(ppu, sprites, count);

    PictureUnit* pu = &ppu->p_unit;
    const uint8_t mask = ppu->mask;
    const uint8_t fine_x = ppu->x;
    const uint8_t bg_table = (ppu->ctrl & BG_TABLE) != 0;
    const uint8_t grey = mask & 0x1 ? 0x30 : 0x3f;
    uint32_t* out = ppu->screen + ppu->scanlines * 256;
    uint16_t pattern_LSB = pu->pattern_LSB;
    uint16_t pattern_MSB = pu->pattern_MSB;
    uint16_t attr_LSB = pu->attr_LSB;
    uint16_t attr_MSB = pu->attr_MSB;
    uint16_t v = ppu->v;
    uint16_t bus = ppu->bus;
    uint8_t status = ppu->status;
    Mapper* mapper = ppu->mapper;
    const uint8_t hooked = mapper->set_bus != mapper_default_set_bus;
    const uint8_t* chr = mapper->read_CHR == mapper_default_read_CHR ? mapper->CHR_ROM : NULL;

    for (size_t dots = first; dots <= last; dots++) {
        uint8_t sprite_pixel = sprites[dots - first];
        if (!(mask & SHOW_SPRITE) || (!(mask & SHOW_SPRITE_8) && dots <= 8))
            sprite_pixel = 0;
        uint8_t bg_pixel = 0;
        if ((mask & SHOW_BG) && ((mask & SHOW_BG_8) || dots > 8)) {
            const unsigned shift = 15u - fine_x;
            const uint8_t pattern = (pattern_LSB >> shift & 1) | (pattern_MSB >> shift & 1) << 1;
            if (pattern)
                bg_pixel = ((attr_LSB >> shift & 1) | (attr_MSB >> shift & 1) << 1) << 2 | pattern;
        }
        uint8_t palette_index;
        if (bg_pixel) {
            palette_index = bg_pixel;
            if (sprite_pixel) {
                if (sprite_pixel & BIT_6 && dots < 256)
                    status |= SPRITE_0_HIT;
                if (!(sprite_pixel & BIT_5))
                    palette_index = sprite_pixel & 0x1f;
            }
        } else
            palette_index = sprite_pixel & 0x1f;
        out[dots - 1] = nes_palette[ppu->palette[palette_index] & grey];

        pattern_LSB = pattern_LSB << 1 | 1;
        pattern_MSB = pattern_MSB << 1 | 1;
        attr_LSB = attr_LSB << 1 | 1;
        attr_MSB = attr_MSB << 1 | 1;

        switch ((dots - 1) & 7) {
            case NT_ADDR:
                bus = 0x2000 | v & 0xFFF;
                if (hooked)
                    mapper->set_bus(mapper, bus);
                break;
            case NT_READ:
                bus = bus & 0xff00 | fetch_vram(ppu, bus);
                pu->NT = bus & 0xff;
                break;
            case AT_ADDR:
                bus = 0x23C0 | v & 0x0C00 | v >> 4 & 0x38 | v >> 2 & 0x07;
                if (hooked)
                    mapper->set_bus(mapper, bus);
                break;
            case AT_READ:
                bus = bus & 0xff00 | fetch_vram(ppu, bus);
                pu->AT = (uint8_t)(bus & 0xFF) >> (v >> 4 & 4 | v & 2);
                break;
            case BG_LSB_ADDR:
                bus = pu->NT << 4 | v >> 12 & 0x7 | bg_table << 12;
                ppu->last_addr = bus;
                if (hooked)
                    mapper->set_bus(mapper, bus);
                break;
            case BG_LSB_READ:
                bus = bus & 0xff00 | (chr && bus < 0x2000 ? chr[bus] : fetch_vram(ppu, bus));
                pu->BG_LSB = bus & 0xff;
                break;
            case BG_MSB_ADDR:
                bus = ppu->last_addr + 8;
                if (hooked)
                    mapper->set_bus(mapper, bus);
                break;
            case BG_MSB_READ:
                bus = bus & 0xff00 | (chr && bus < 0x2000 ? chr[bus] : fetch_vram(ppu, bus));
                pu->BG_MSB = bus & 0xff;
                ppu->v = v;
                if (dots == 256)
                    inc_vert_v(ppu);
                inc_hori_v(ppu);
                v = ppu->v;
                pattern_LSB = pattern_LSB & 0xff00 | pu->BG_LSB;
                pattern_MSB = pattern_MSB & 0xff00 | pu->BG_MSB;
                attr_LSB = attr_LSB & 0xff00 | (pu->AT & 0b01 ? 0xff : 0);
                attr_MSB = attr_MSB & 0xff00 | (pu->AT & 0b10 ? 0xff : 0);
                break;
        }
    }
    pu->pattern_LSB = pattern_LSB;
    pu->pattern_MSB = pattern_MSB;
    pu->attr_LSB = attr_LSB;
    pu->attr_MSB = attr_MSB;
    pu->has_set_addr = 0;
    ppu->v = v;
    ppu->bus = bus;
    ppu->status = status;
    count_down_latch(ppu, count);
    ppu->dots = last + 1;
}

void execute_ppu(PPU* ppu) {
    if (can_render_span(ppu)) {
        execute_render_dot(ppu, ppu->dots);
        return;
    }
    execute_dot(ppu);
}

static uint8_t can_fetch_span(const PPU* ppu) {
    return ppu->dots > VISIBLE_DOTS && ppu->scanlines < VISIBLE_SCANLINES && ppu->render_status && is_quiet(ppu);
}

static void count_down_unit(SpriteUnit* unit, size_t count) {
    if (unit->halted || !unit->x || !count)
        return;
    if (unit->x > count) {
        unit->x -= (uint8_t)count;
        return;
    }
    unit->x = 0;
    unit->halted = 1;
}

static void fetch_span(PPU* ppu, size_t count) {
    const size_t first = ppu->dots;
    const size_t last = first + count - 1;

    for (int i = 0; i < 8; i++) {
        const size_t fetch_dot = 260 + 8 * (size_t)i;
        count_down_unit(ppu->sprite_units + i, fetch_dot >= first && fetch_dot <= last ? fetch_dot - first : count);
    }

    PictureUnit* pu = &ppu->p_unit;
    Mapper* mapper = ppu->mapper;
    const uint8_t hooked = mapper->set_bus != mapper_default_set_bus;
    const uint8_t* cache = ppu->OAM_cache;
    const uint8_t line = ppu->scanlines & 0xff;
    const uint8_t ctrl = ppu->ctrl;
    const unsigned height = 8u << (ctrl & LONG_SPRITE ? 1 : 0);
    Sprite* sb = &ppu->sprite_buffer;
    uint16_t v = ppu->v;
    uint16_t bus = ppu->bus;
    uint8_t sec = ppu->sec_oam_address;
    uint8_t buffer = ppu->sprite_eval_unit.buffer;

    for (size_t dots = first; dots <= last; dots++) {
        const uint8_t sprite_prefetch = dots <= 320;
        const PPUPhase phase = (dots - 1) & 7;
        SpriteUnit* unit = ppu->sprite_units + ((dots - 257) >> 3 & 7);

        if (dots > 320 && dots < 337) {
            pu->pattern_LSB = pu->pattern_LSB << 1 | 1;
            pu->pattern_MSB = pu->pattern_MSB << 1 | 1;
            pu->attr_LSB  = pu->attr_LSB << 1 | 1;
            pu->attr_MSB = pu->attr_MSB << 1 | 1;
        }
        if (sprite_prefetch)
            ppu->oam_address = 0;
        else
            buffer = cache[sec & 0x1f];

        switch (phase) {
            case NT_ADDR:
                bus = 0x2000 | v & 0xFFF;
                if (dots == 257) {
                    v &= ~HORIZONTAL_BITS;
                    v |= ppu->t & HORIZONTAL_BITS;
                }
                if (sprite_prefetch) {
                    sb->y = buffer = cache[sec & 0x1f];
                    if (sec < 32)
                        sec++;
                }
                break;
            case NT_READ:
                bus = bus & 0xff00 | fetch_vram(ppu, bus);
                pu->NT = bus & 0xff;
                if (sprite_prefetch) {
                    sb->tile = buffer = cache[sec & 0x1f];
                    if (sec < 32)
                        sec++;
                }
                break;
            case AT_ADDR:
                if (sprite_prefetch || dots >= 337)
                    bus = 0x2000 | v & 0xFFF;
                else
                    bus = 0x23C0 | v & 0x0C00 | v >> 4 & 0x38 | v >> 2 & 0x07;
                if (sprite_prefetch) {
                    sb->attr = buffer = cache[sec & 0x1f];
                    if (sec < 32)
                        sec++;
                    unit->attr = sb->attr | (dots == 259 && ppu->sprite_eval_unit.has_sprite_zero ? BIT_2 : 0);
                }
                break;
            case AT_READ:
                bus = bus & 0xff00 | fetch_vram(ppu, bus);
                pu->AT = (uint8_t)(bus & 0xFF) >> (v >> 4 & 4 | v & 2);
                if (sprite_prefetch) {
                    sb->x = buffer = cache[sec & 0x1f];
                    unit->x = sb->x;
                    unit->halted = 1;
                }
                break;
            case BG_LSB_ADDR:
                if (sprite_prefetch) {
                    uint8_t offset = line - sb->y;
                    if (ctrl & LONG_SPRITE) {
                        if (sb->attr & FLIP_VERTICAL)
                            offset ^= 15;
                        bus = (((sb->tile & 0x1) << 12) | ((sb->tile & ~0x1) << 4)) + ((offset & 0x8) << 1) + (offset & 0x7);
                    } else {
                        if (sb->attr & FLIP_VERTICAL)
                            offset ^= 7;
                        bus = ((ctrl & SPRITE_TABLE ? 0x1000: 0) | (sb->tile << 4)) + (offset & 0x7);
                    }
                } else {
                    bus = pu->NT << 4 | v >> 12 & 0x7 | (ctrl & BG_TABLE) << 8;
                }
                ppu->last_addr = bus;
                break;
            case BG_LSB_READ:
                bus = bus & 0xff00 | fetch_vram(ppu, bus);
                if (sprite_prefetch)
                    unit->pattern_LSB = bus & 0xff;
                else
                    pu->BG_LSB = bus & 0xff;
                break;
            case BG_MSB_ADDR:
                bus = ppu->last_addr + 8;
                break;
            case BG_MSB_READ:
                bus = bus & 0xff00 | fetch_vram(ppu, bus);
                if (sprite_prefetch) {
                    unit->pattern_MSB = bus & 0xff;
                    if (sec < 32)
                        sec++;
                    if (!in_sprite_range(line, sb->y, height)) {
                        unit->pattern_MSB = 0;
                        unit->pattern_LSB = 0;
                    }
                } else {
                    pu->BG_MSB = bus & 0xff;
                    ppu->v = v;
                    inc_hori_v(ppu);
                    v = ppu->v;
                }
                pu->pattern_LSB = pu->pattern_LSB & 0xff00 | pu->BG_LSB;
                pu->pattern_MSB = pu->pattern_MSB & 0xff00 | pu->BG_MSB;
                pu->attr_LSB = pu->attr_LSB & 0xff00 | (pu->AT & 0b01 ? 0xff : 0) ;
                pu->attr_MSB = pu->attr_MSB & 0xff00 | (pu->AT & 0b10 ? 0xff : 0) ;
                break;
        }

        if (hooked && !(phase & 1))
            mapper->set_bus(mapper, bus);

        if (dots == 340)
            sec = 0;
    }

    ppu->v = v;
    ppu->bus = bus;
    ppu->sec_oam_address = sec;
    ppu->sprite_eval_unit.buffer = buffer;
    pu->has_set_addr = 0;
    if (last == END_DOT)
        ppu->shift_start_delay = 1;
    count_down_latch(ppu, count);
    if (last == END_DOT) {
        ppu->dots = 0;
        ppu->scanlines++;
    } else
        ppu->dots = last + 1;
}

static uint8_t can_idle_span(const PPU* ppu) {
    return ppu->scanlines > 241 && ppu->scanlines < ppu->pre_render
        && !(ppu->render_state_delay | ppu->should_inc_v | ppu->read_to_buffer
            | ppu->write_to_buffer | ppu->nmi_delay);
}

static void idle_span(PPU* ppu, size_t count) {
    ppu->p_unit.has_set_addr = 0;
    count_down_latch(ppu, count);
    ppu->dots += count;
}

static void run_dots(PPU* ppu, size_t count) {
    while (count) {
        size_t span;
        if (can_render_span(ppu)) {
            span = VISIBLE_DOTS + 1 - ppu->dots;
            if (span > count)
                span = count;
            render_span(ppu, span);
        } else if (can_fetch_span(ppu)) {
            span = DOTS_PER_SCANLINE - ppu->dots;
            if (span > count)
                span = count;
            fetch_span(ppu, span);
        } else if (can_idle_span(ppu) && ppu->dots < END_DOT) {
            span = END_DOT - ppu->dots;
            if (span > count)
                span = count;
            idle_span(ppu, span);
        } else {
            execute_dot(ppu);
            span = 1;
        }
        count -= span;
    }
}

typedef enum DotDeferral {
    NO_DEFERRAL = 0,
    DEFER_DOTS,
    DEFER_DOTS_UNTIL_A12_HIGH,
} DotDeferral;

static size_t dots_to_a12_high(const PPU* ppu) {
    if (ppu->should_inc_v | ppu->read_to_buffer | ppu->write_to_buffer | ppu->render_state_delay)
        return 1;
    if (!ppu->render_status)
        return SIZE_MAX;
    if (ppu->ctrl & BG_TABLE)
        return 1;
    if (!(ppu->ctrl & (SPRITE_TABLE | LONG_SPRITE)))
        return SIZE_MAX;
    const size_t scanlines = ppu->scanlines;
    const size_t dots = ppu->dots;
    if ((scanlines < VISIBLE_SCANLINES || scanlines == ppu->pre_render) && dots <= 320)
        return dots >= 257 ? 1 : 257 - dots + 1;
    if (scanlines + 1 < VISIBLE_SCANLINES)
        return DOTS_PER_SCANLINE - dots + 257 + 1;
    if (scanlines < ppu->pre_render)
        return (ppu->pre_render - scanlines) * DOTS_PER_SCANLINE + 257 - dots + 1;
    return SIZE_MAX;
}

static size_t dots_to_next_event(const PPU* ppu) {
    if (ppu->nmi_delay)
        return 1;
    const size_t position = ppu->scanlines * DOTS_PER_SCANLINE + ppu->dots;
    const size_t vblank = 241 * DOTS_PER_SCANLINE + 1;
    const size_t pre_render = (size_t)ppu->pre_render * DOTS_PER_SCANLINE;
    size_t budget;
    if (position <= vblank)
        budget = vblank - position + 1;
    else if (position <= pre_render + 1)
        budget = pre_render + 1 - position + 1;
    else
        budget = pre_render + END_DOT - position + 1;
    if (ppu->defer_dots == DEFER_DOTS_UNTIL_A12_HIGH) {
        const size_t a12 = dots_to_a12_high(ppu);
        if (a12 < budget)
            budget = a12;
    }
    return budget;
}

static uint8_t can_defer_dots(const Mapper* mapper) {
    if (mapper->is_nsf)
        return NO_DEFERRAL;
    if (mapper->set_bus != mapper_default_set_bus)
        return mapper->mapper_num == 4 && mapper->submapper != 3 ? DEFER_DOTS_UNTIL_A12_HIGH : NO_DEFERRAL;
    switch (mapper->mapper_num) {
        case 0: case 1: case 2: case 3: case 7: case 11: case 13: case 18:
        case 21: case 22: case 23: case 25: case 46: case 66: case 75: case 94: case 180:
            return DEFER_DOTS;
        default:
            return NO_DEFERRAL;
    }
}

void clock_ppu(PPU* ppu, uint8_t dots) {
    if (!ppu->defer_dots) {
        while (dots--)
            execute_ppu(ppu);
        return;
    }
    ppu->pending_dots += dots;
    if (ppu->pending_dots >= ppu->dot_budget) {
        run_dots(ppu, ppu->pending_dots);
        ppu->pending_dots = 0;
        ppu->dot_budget = dots_to_next_event(ppu);
    }
}

void sync_ppu(PPU* ppu) {
    if (ppu->pending_dots) {
        run_dots(ppu, ppu->pending_dots);
        ppu->pending_dots = 0;
    }
    ppu->dot_budget = 0;
}

void begin_ppu_batch(PPU* ppu) {
    ppu->pending_dots = 0;
    ppu->dot_budget = 0;
    ppu->defer_dots = can_defer_dots(ppu->mapper);
}

void end_ppu_batch(PPU* ppu) {
    sync_ppu(ppu);
    ppu->defer_dots = NO_DEFERRAL;
}

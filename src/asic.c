#include "asic.h"

#include <string.h>
#include "crtc.h"
#include "display.h"
#include "gate_array.h"
#include "mem.h"
#include "psg.h"

static u8 decode_magnification(u8 value) {
    value &= 3;
    return value == 3 ? 4 : value;
}

static u32 decode_colour(const u8 *registers, unsigned index) {
    u8 rb = registers[0x2400 + index * 2];
    u8 g = registers[0x2401 + index * 2] & 0x0F;
    u8 r = rb >> 4;
    u8 b = rb & 0x0F;
    return ((u32)(r * 17) << 16) | ((u32)(g * 17) << 8) | (u32)(b * 17);
}

void asic_reset(Asic *asic, GateArray *ga) {
    memset(asic, 0, sizeof(*asic));
    asic->interrupt_vector = 1;
    for (int i = 0; i < GA_NUM_INKS; i++)
        asic->palette[i] = ga->resolved_ink[i];
}

void asic_register_write(Asic *asic, GateArray *ga, Mem *mem,
                         u16 addr, u8 value) {
    if (addr < 0x4000 || addr >= 0x8000) return;
    u16 off = addr - 0x4000;

    if (addr < 0x5000) {
        int id = (addr >> 8) & 0x0F;
        int y = (addr >> 4) & 0x0F;
        int x = addr & 0x0F;
        value &= 0x0F;
        mem->plus_registers[off] = value;
        asic->sprite[id][x][y] = value;
        return;
    }

    if (addr >= 0x6000 && addr < 0x6080) {
        int id = (addr - 0x6000) >> 3;
        int reg = addr & 7;
        if (id >= ASIC_SPRITE_COUNT) return;
        switch (reg) {
        case 0:
            asic->sprite_x[id] = (asic->sprite_x[id] & 0x0300) | value;
            mem->plus_registers[off + 4] = value;
            break;
        case 1:
            value &= 3;
            asic->sprite_x[id] = (asic->sprite_x[id] & 0x00FF) | ((u16)value << 8);
            mem->plus_registers[off + 4] = value;
            break;
        case 2:
            asic->sprite_y[id] = (asic->sprite_y[id] & 0x0100) | value;
            mem->plus_registers[off + 4] = value;
            break;
        case 3:
            value &= 1;
            asic->sprite_y[id] = (asic->sprite_y[id] & 0x00FF) | ((u16)value << 8);
            mem->plus_registers[off + 4] = value;
            break;
        case 4:
            asic->sprite_mag_x[id] = decode_magnification(value >> 2);
            asic->sprite_mag_y[id] = decode_magnification(value);
            return; /* magnification is write-only */
        default:
            break;
        }
        mem->plus_registers[off] = value;
        return;
    }

    if (addr >= 0x6400 && addr < 0x6440) {
        unsigned colour = (addr - 0x6400) >> 1;
        if (addr & 1) value &= 0x0F;
        mem->plus_registers[off] = value;
        asic->palette[colour] = decode_colour(mem->plus_registers, colour);
        asic->palette_set[colour] = true;
        if (colour < GA_NUM_INKS)
            ga->resolved_ink[colour] = asic->palette[colour];
        return;
    }

    mem->plus_registers[off] = value;
    switch (addr) {
    case 0x6800: asic->raster_line = value; break;
    case 0x6801: asic->split_line = value; break;
    case 0x6802:
        asic->split_address = (asic->split_address & 0x00FF) | ((u16)value << 8);
        break;
    case 0x6803:
        asic->split_address = (asic->split_address & 0x3F00) | value;
        break;
    case 0x6804:
        asic->hscroll = value & 0x0F;
        asic->vscroll = (value >> 4) & 7;
        asic->extend_border = (value & 0x80) != 0;
        break;
    case 0x6805: asic->interrupt_vector = value & 0xF8; break;
    case 0x6C00: case 0x6C04: case 0x6C08: {
        int channel = (addr - 0x6C00) >> 2;
        asic->dma[channel].source =
            (asic->dma[channel].source & 0xFF00) | (value & 0xFE);
        break;
    }
    case 0x6C01: case 0x6C05: case 0x6C09: {
        int channel = (addr - 0x6C00) >> 2;
        asic->dma[channel].source =
            (asic->dma[channel].source & 0x00FF) | ((u16)value << 8);
        break;
    }
    case 0x6C02: case 0x6C06: case 0x6C0A: {
        int channel = (addr - 0x6C00) >> 2;
        asic->dma[channel].prescaler = value;
        break;
    }
    case 0x6C0F:
        for (int channel = 0; channel < ASIC_DMA_CHANNELS; channel++) {
            asic->dma[channel].enabled = (value & (1u << channel)) != 0;
            if (value & (0x40u >> channel))
                asic->dma[channel].interrupt = false;
        }
        break;
    default:
        break;
    }
}

static void dma_store_status(Asic *asic, Mem *mem) {
    u8 status = 0;
    for (int i = 0; i < ASIC_DMA_CHANNELS; i++) {
        AsicDmaChannel *channel = &asic->dma[i];
        mem->plus_registers[0x2C00 + i * 4] = channel->source & 0xFF;
        mem->plus_registers[0x2C01 + i * 4] = channel->source >> 8;
        if (channel->enabled) status |= 1u << i;
        if (channel->interrupt) status |= 0x40u >> i;
    }
    mem->plus_registers[0x2C0F] = status;
}

static void dma_cycle(Asic *asic, Mem *mem, PSG *psg, GateArray *ga) {
    for (int i = 0; i < ASIC_DMA_CHANNELS; i++) {
        AsicDmaChannel *channel = &asic->dma[i];
        if (!channel->enabled) continue;
        if (channel->pause) {
            if (channel->prescale_count < channel->prescaler) {
                channel->prescale_count++;
                continue;
            }
            channel->prescale_count = 0;
            channel->pause--;
            continue;
        }

        u16 instruction = (u16)mem_read_dma(mem, channel->source) |
                          ((u16)mem_read_dma(mem, channel->source + 1) << 8);
        u8 opcode = (instruction >> 12) & 7;
        if (opcode == 0) {
            u8 selected = psg->selected;
            psg_select(psg, (instruction >> 8) & 0x0F);
            psg_write(psg, instruction & 0xFF);
            psg->selected = selected;
        } else {
            if (opcode & 1) {
                channel->pause = instruction & 0x0FFF;
                channel->prescale_count = 0;
            }
            if (opcode & 2) {
                channel->loops = instruction & 0x0FFF;
                channel->loop = channel->source;
            }
            if (opcode & 4) {
                if ((instruction & 0x0001) && channel->loops) {
                    channel->source = channel->loop;
                    channel->loops--;
                }
                if (instruction & 0x0010) {
                    channel->interrupt = true;
                    ga->interrupt_pending = true;
                }
                if (instruction & 0x0020)
                    channel->enabled = false;
            }
        }
        channel->source += 2;
    }
    dma_store_status(asic, mem);
}

void asic_hsync(Asic *asic, Mem *mem, PSG *psg, GateArray *ga) {
    bool pending = ga->interrupt_pending;

    asic->scanline++;
    ga_hsync(ga);
    if (asic->raster_line)
        ga->interrupt_pending = pending;
    if (asic->raster_line && (u8)asic->scanline == asic->raster_line)
        ga->interrupt_pending = true;
    dma_cycle(asic, mem, psg, ga);
}

void asic_new_frame(Asic *asic) {
    asic->scanline = 0;
}

void asic_apply_split(const Asic *asic, CRTC *crtc) {
    if (!asic->split_line || (u8)asic->scanline != asic->split_line)
        return;
    crtc->ma = asic->split_address & 0x3FFF;
    crtc->ma_row_start = crtc->ma;
    crtc->ma_next_row = crtc->ma;
}

void asic_draw_sprites(const Asic *asic, const CRTC *crtc, Display *display) {
    /* Plus sprite coordinates are relative to a 640x200 display with the
     * normal 64/40-pixel border origin. Lower-numbered sprites have priority,
     * so composite from 15 down to 0. */
    for (int id = ASIC_SPRITE_COUNT - 1; id >= 0; id--) {
        int mx = asic->sprite_mag_x[id];
        int my = asic->sprite_mag_y[id];
        if (!mx || !my) continue;
        int border_x = 64 + (asic->extend_border ? 16 : 0);
        int border_y = 40 + 8 * (30 - crtc->reg[7]);
        if (border_y < 0) border_y = 0;
        int right = border_x + 640;
        int bottom = border_y + 200;
        int sx = (int)asic->sprite_x[id] + border_x;
        int sy = (int)asic->sprite_y[id] + border_y;
        for (int y = 0; y < 16; y++) {
            for (int x = 0; x < 16; x++) {
                u8 pen = asic->sprite[id][x][y] & 0x0F;
                if (!pen) continue;
                u32 colour = asic->palette[16 + pen];
                for (int dy = 0; dy < my; dy++) {
                    int py = sy + y * my + dy;
                    if (py <= border_y || py >= bottom) continue;
                    for (int dx = 0; dx < mx; dx++) {
                        int px = sx + x * mx + dx;
                        if (px > border_x && px < right)
                            display->pixels[py * CPC_SCREEN_W + px] = colour;
                    }
                }
            }
        }
    }
}

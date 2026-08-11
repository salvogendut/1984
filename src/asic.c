#include "asic.h"

#include <string.h>
#include "crtc.h"
#include "gate_array.h"
#include "mem.h"
#include "psg.h"

static void dma_store_status(Asic *asic, Mem *mem);

static void raise_raster_interrupt(Asic *asic, Mem *mem, GateArray *ga) {
    asic->raster_interrupt = true;
    ga->interrupt_pending = true;
    dma_store_status(asic, mem);
}

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
        case 4: case 5: case 6: case 7:
            /* A1-A0 are not decoded for magnification writes:
             * all four trailing offsets select the same register. Reads
             * still mirror X/Y through plus_registers, populated above. */
            asic->sprite_mag_x[id] = decode_magnification(value >> 2);
            asic->sprite_mag_y[id] = decode_magnification(value);
            return; /* magnification is write-only */
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
    case 0x6800: {
        u8 previous = asic->raster_line;
        asic->raster_line = value;
        /* PRI is a level-sensitive request. Programming a different nonzero
         * line withdraws an outstanding raster request unless the new value
         * matches immediately (handled by asic_program_raster()). Eerie
         * Forest relies on the withdrawal while rebuilding its IRQ chain. */
        if (value && value != previous) {
            asic->raster_interrupt = false;
            ga->interrupt_pending = asic_irq_pending(asic);
            dma_store_status(asic, mem);
        }
        break;
    }
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
    case 0x6805:
        /* D7-D3 form the vector base. D0 controls automatic DMA IRQ
         * acknowledgement; D2-D1 are supplied by the interrupt source. */
        asic->interrupt_vector = value & 0xF9;
        break;
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
        dma_store_status(asic, mem);
        break;
    default:
        break;
    }
}

void asic_program_raster(Asic *asic, GateArray *ga, Mem *mem,
                         const CRTC *crtc, u8 value) {
    u8 previous = asic->raster_line;
    asic_register_write(asic, ga, mem, 0x6800, value);
    if (!crtc || !value || value == previous || !crtc->hsync ||
        crtc->vcc >= 32)
        return;

    u8 line = (u8)((crtc->vcc << 3) | (crtc->vlc & 7));
    if (line == value)
        raise_raster_interrupt(asic, mem, ga);
}

static void dma_store_status(Asic *asic, Mem *mem) {
    /* DCSR bit 7 reports the source of the last interrupt acknowledge; it is
     * not the live programmable-raster request.  IM1 handlers use this latch
     * to distinguish raster and DMA interrupts after the request is cleared. */
    u8 status = asic->raster_acknowledged ? 0x80 : 0;
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
                /* PAUSE execution is part of the requested delay. Prime the
                 * prescaler so the following instruction is fetched after
                 * exactly pause * (prescaler + 1) HSYNC periods. */
                if (channel->pause && !channel->prescaler)
                    channel->pause--;
                channel->prescale_count = channel->prescaler ? 1 : 0;
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
    bool legacy_raster = !pending && ga->interrupt_pending;
    if (asic->raster_line) {
        ga->interrupt_pending = pending;
    } else if (legacy_raster) {
        asic->raster_interrupt = true;
    }
    dma_cycle(asic, mem, psg, ga);
}

void asic_raster_tick(Asic *asic, const CRTC *crtc, Mem *mem, GateArray *ga) {
    if (!asic->raster_line)
        return;

    /* The programmable interrupt comparator is sampled 10 character clocks
     * after HSYNC starts, independently of the programmed HSYNC width. */
    u8 irq_hcc = (u8)(crtc->reg[2] + 10);
    if ((u8)crtc->hcc != irq_hcc)
        return;

    /* PRI/SSSL contain a five-bit character-row comparator. The live CRTC
     * counter is wider; rows 32 and above do not wrap around and match rows
     * 0..31. */
    if (crtc->vcc >= 32)
        return;
    u8 line = (u8)((crtc->vcc << 3) | (crtc->vlc & 7));
    if (line == asic->raster_line)
        raise_raster_interrupt(asic, mem, ga);
}

void asic_new_frame(Asic *asic) {
    asic->scanline = 0;
    asic->video_ma_offset = 0;
    asic->video_next_valid = false;
}

bool asic_irq_pending(const Asic *asic) {
    if (asic->raster_interrupt) return true;
    for (int i = 0; i < ASIC_DMA_CHANNELS; i++)
        if (asic->dma[i].interrupt) return true;
    return false;
}

u8 asic_irq_vector(const Asic *asic) {
    u8 source = 6; /* Raster IRQ, including the legacy 52-HSYNC source. */
    if (!asic->raster_interrupt) {
        if (asic->dma[2].interrupt) source = 0;
        else if (asic->dma[1].interrupt) source = 2;
        else if (asic->dma[0].interrupt) source = 4;
    }
    return (asic->interrupt_vector & 0xF8) | source;
}

void asic_irq_ack(Asic *asic, Mem *mem, GateArray *ga) {
    if (asic->raster_interrupt) {
        asic->raster_interrupt = false;
        asic->raster_acknowledged = true;
        ga_irq_ack(ga);
    } else {
        asic->raster_acknowledged = false;
        /* DMA priority is channel 2, then 1, then 0. With IVR bit 0 clear,
         * the acknowledged channel is automatically removed from DCSR. */
        if (!(asic->interrupt_vector & 1)) {
            for (int i = ASIC_DMA_CHANNELS - 1; i >= 0; i--) {
                if (asic->dma[i].interrupt) {
                    asic->dma[i].interrupt = false;
                    break;
                }
            }
        }
    }
    dma_store_status(asic, mem);
    ga->interrupt_pending = asic_irq_pending(asic);
}

void asic_clear_raster_irq(Asic *asic, Mem *mem) {
    asic->raster_interrupt = false;
    dma_store_status(asic, mem);
}

void asic_latch_split(Asic *asic, const CRTC *crtc, u16 previous_vcc,
                      u16 previous_vlc, bool new_scanline) {
    u16 vcc = new_scanline ? previous_vcc : crtc->vcc;
    u16 vlc = new_scanline ? previous_vlc : crtc->vlc;
    if (vcc >= 32)
        return;
    u8 line = (u8)((vcc << 3) | (vlc & 7));
    if (!asic->split_line || line != asic->split_line)
        return;

    /* Normally SSA is sampled when HCC reaches R1. Software may also program
     * SSSL while its comparator line is already active; if R1 has passed,
     * sample SSA at the end of that line. An R1 sample keeps priority so
     * later writes can prepare another split without changing this one. On
     * the final raster of the frame only the end-of-line sample is valid. */
    bool final_raster = vcc == crtc->reg[4] && vlc == crtc->reg[9];
    bool latch = new_scanline ? (final_raster || !asic->split_pending)
                              : (!final_raster &&
                                 crtc->hcc == crtc->reg[1]);
    if (!latch)
        return;

    asic->split_pending_base = asic->split_address & 0x3FFF;
    asic->split_pending = true;
}

void asic_latch_video_row(Asic *asic, const CRTC *crtc) {
    u16 width = crtc->reg[1];
    if (!width || crtc->hcc != width)
        return;

    /* SSCR changes the three raster-address bits immediately, but its carry
     * into the next video-memory row is sampled only by the R1 comparator.
     * Keeping this row base stateful matters when software rewrites SSCR near
     * the start of every raster, as Eerie Forest does for its lower panel. */
    u16 raw_start = (u16)((crtc->ma - crtc->hcc) & 0x3FFF);
    u16 video_start = (u16)((raw_start + asic->video_ma_offset) & 0x3FFF);
    unsigned scrolled_raster = (crtc->vlc + asic->vscroll) & 0x1F;
    if (scrolled_raster == crtc->reg[9])
        video_start = (u16)((video_start + width) & 0x3FFF);
    asic->video_next_base = video_start;
    asic->video_next_valid = true;
}

void asic_apply_split(Asic *asic, const CRTC *crtc) {
    u16 raw_start = (u16)((crtc->ma - crtc->hcc) & 0x3FFF);
    u16 video_start;

    if (asic->split_pending) {
        /* SSA has priority over the row advance sampled at R1. */
        video_start = asic->split_pending_base;
        asic->split_pending = false;
    } else if (asic->video_next_valid) {
        video_start = asic->video_next_base;
    } else {
        video_start = (u16)((raw_start + asic->video_ma_offset) & 0x3FFF);
    }

    asic->video_ma_offset = (u16)((video_start - raw_start) & 0x3FFF);
    asic->video_next_valid = false;
}

u16 asic_video_ma(const Asic *asic, u16 crtc_ma) {
    return (u16)((crtc_ma + asic->video_ma_offset) & 0x3FFF);
}

AsicVideoPosition asic_video_position(const Asic *asic, u16 crtc_ma,
                                      u8 crtc_raster) {
    AsicVideoPosition pos = {
        .ma = asic_video_ma(asic, crtc_ma),
        .raster = (u8)((crtc_raster + asic->vscroll) & 7),
    };
    return pos;
}

bool asic_scroll_border_active(const Asic *asic, u16 hcc) {
    /* SSCR bit 7 masks the invalid data exposed by horizontal scrolling by
     * extending the left border over the first CRTC character. */
    return asic->extend_border && hcc == 0;
}

void asic_draw_sprites_char(const Asic *asic, u16 hcc, u16 vcc, u16 vlc,
                            u8 chars_per_row, u32 *pixels) {
    int sprite_row[ASIC_SPRITE_COUNT];
    unsigned beam_y = (((unsigned)vcc & 0x3F) << 3) | (vlc & 7);

    /* Sprite coordinates are compared directly with CRTC counters while the
     * beam is drawing: Plus software can change sprite data and attributes
     * several times within one frame. There is no vertical clip here — the
     * caller only invokes this for scanlines the monitor beam actually draws,
     * and CRTC-counter based sprites may legitimately sit in the bottom
     * border on overscan screens (e.g. GNG's 248-line mode). */
    for (int id = 0; id < ASIC_SPRITE_COUNT; id++) {
        int mx = asic->sprite_mag_x[id];
        int my = asic->sprite_mag_y[id];
        unsigned rel_y = (beam_y - asic->sprite_y[id]) & 0x1FF;
        sprite_row[id] = mx && my && rel_y < (unsigned)(16 * my)
            ? (int)(rel_y / (unsigned)my) : -1;
    }

    /* Sprite X coordinates use the 640-pixel (mode 2) clock: a CRTC character
     * spans 16 coordinates. The visible sprite window is the CRTC display
     * width (R1) in counter space, matching MAME, which clips sprites to the
     * display-enable area: sprites enter and leave exactly at the playfield
     * edges and never bleed into the side borders. Sprite 0 has highest
     * priority, so the first opaque sprite wins. */
    unsigned beam_x = ((unsigned)hcc * 16) & 0x3FF;
    for (int x = 0; x < 16; x++, beam_x = (beam_x + 1) & 0x3FF) {
        if ((int)beam_x >= (int)chars_per_row * 16)
            continue;
        for (int id = 0; id < ASIC_SPRITE_COUNT; id++) {
            int mx = asic->sprite_mag_x[id];
            if (sprite_row[id] < 0)
                continue;
            unsigned rel_x = (beam_x - asic->sprite_x[id]) & 0x3FF;
            if (rel_x >= (unsigned)(16 * mx))
                continue;
            u8 pen = asic->sprite[id][rel_x / (unsigned)mx]
                                  [sprite_row[id]] & 0x0F;
            if (pen) {
                pixels[x] = asic->palette[16 + pen];
                break;
            }
        }
    }
}

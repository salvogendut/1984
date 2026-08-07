#pragma once

#include <stdbool.h>
#include "types.h"
#include "crtc.h"
#include "gate_array.h"
#include "mem.h"
#include "psg.h"

#define ASIC_SPRITE_COUNT 16
#define ASIC_DMA_CHANNELS 3

typedef struct {
    u16 source;
    u16 loop;
    u16 pause;
    u16 loops;
    u8  prescaler;
    u8  prescale_count;
    bool enabled;
    bool interrupt;
} AsicDmaChannel;

typedef struct {
    u8 sprite[ASIC_SPRITE_COUNT][16][16];
    u16 sprite_x[ASIC_SPRITE_COUNT];
    u16 sprite_y[ASIC_SPRITE_COUNT];
    u8 sprite_mag_x[ASIC_SPRITE_COUNT];
    u8 sprite_mag_y[ASIC_SPRITE_COUNT];
    u32 palette[32];
    bool palette_set[32];

    u8 raster_line;
    u8 split_line;
    u16 split_address;
    u16 split_pending_base;
    u16 split_ma_started;
    u16 split_ma_base;
    bool split_pending;
    bool split_active;
    u8 hscroll;
    u8 vscroll;
    bool extend_border;
    u8 interrupt_vector;
    bool raster_interrupt;
    u16 scanline;

    AsicDmaChannel dma[ASIC_DMA_CHANNELS];
} Asic;

typedef struct {
    u16 ma;
    u8 raster;
} AsicVideoPosition;

void asic_reset(Asic *asic, GateArray *ga);
void asic_register_write(Asic *asic, GateArray *ga, Mem *mem,
                         u16 addr, u8 value);
void asic_hsync(Asic *asic, Mem *mem, PSG *psg, GateArray *ga);
void asic_raster_tick(Asic *asic, const CRTC *crtc, Mem *mem, GateArray *ga);
void asic_new_frame(Asic *asic);
bool asic_irq_pending(const Asic *asic);
u8 asic_irq_vector(const Asic *asic);
void asic_irq_ack(Asic *asic, Mem *mem, GateArray *ga);
void asic_clear_raster_irq(Asic *asic, Mem *mem);
void asic_latch_split(Asic *asic, const CRTC *crtc, u16 previous_vcc,
                      u16 previous_vlc, bool new_scanline);
void asic_apply_split(Asic *asic, const CRTC *crtc);
u16 asic_video_ma(const Asic *asic, u16 crtc_ma);
AsicVideoPosition asic_video_position(const Asic *asic, u16 crtc_ma,
                                      u8 crtc_raster, u8 max_raster,
                                      u8 chars_per_row);
void asic_draw_sprites_char(const Asic *asic, u16 hcc, u16 vcc, u16 vlc,
                            u32 *pixels);

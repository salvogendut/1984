#pragma once

#include <stdbool.h>
#include "types.h"
#include "crtc.h"
#include "display.h"
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
    u8 hscroll;
    u8 vscroll;
    bool extend_border;
    u8 interrupt_vector;
    u16 scanline;

    AsicDmaChannel dma[ASIC_DMA_CHANNELS];
} Asic;

void asic_reset(Asic *asic, GateArray *ga);
void asic_register_write(Asic *asic, GateArray *ga, Mem *mem,
                         u16 addr, u8 value);
void asic_hsync(Asic *asic, Mem *mem, PSG *psg, GateArray *ga);
void asic_new_frame(Asic *asic);
void asic_apply_split(const Asic *asic, CRTC *crtc);
void asic_draw_sprites(const Asic *asic, const CRTC *crtc, Display *display);

#pragma once
#include "types.h"
#include "z80.h"
#include "mem.h"
#include "gate_array.h"
#include "crtc.h"
#include "ppi.h"
#include "psg.h"
#include "kbd.h"
#include "display.h"
#include "fdc.h"
#include "disk.h"

typedef enum { MODEL_464, MODEL_6128 } CpcModel;

typedef struct {
    CpcModel   model;
    Z80        cpu;
    Z80Bus     bus;
    Mem        mem;
    GateArray  ga;
    CRTC       crtc;
    PPI        ppi;
    PSG        psg;
    Keyboard   kbd;
    Display    display;
    SDL_AudioStream *audio_stream;
    Disk       drive[2];    /* drive[0]=A, drive[1]=B */
    FDC        fdc;

    /* Timing */
    int  cpu_clk_hz;      /* 4 MHz */
    int  cycles_per_frame;
    int  cycle_debt;      /* leftover cycles from previous frame */

    /* Raster position (in character-clock units; 16 output pixels each) */
    int  raster_x;        /* 0 = first char after hsync end */
    int  raster_y;        /* 0 = first scanline after vsync */
    bool prev_hsync;
    bool prev_vsync;
} CPC;

extern int cpc_trace_io;      /* set to 1 to log CRTC/GA writes to stderr */
extern int cpc_trace_palette; /* set to 1 to log palette buffer state when B7F7=0xFF */
extern int cpc_frame_count;   /* incremented by cpc_frame(); used by trace helpers */

int  cpc_init(CPC *cpc, CpcModel model, const char *rom_os, const char *rom_basic);
void cpc_reset(CPC *cpc);
void cpc_destroy(CPC *cpc);
void cpc_frame(CPC *cpc);        /* run one video frame (~20 ms) */
void cpc_key_event(CPC *cpc, SDL_Scancode sc, bool pressed);

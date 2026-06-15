#pragma once
#include <stdbool.h>
#include <stdlib.h>     /* dbg_getenv() wraps getenv() */
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
#include "rtc.h"
#include "ide.h"
#include "mouse.h"
#include "m4.h"
#include "symbnet.h"
#include "ch376.h"
#include "tape.h"

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
    bool       mx4;           /* MX4 expansion bus (all extension I/O gated on this) */
    bool       net4cpc;       /* Net4CPC W5100S Ethernet add-on present */
    bool       rtc;           /* Real-time clock add-on present */
    RTC        rtc_chip;
    bool       symbiface_ide;   /* SYMBiFACE II / Cyboard IDE add-on present */
    IDE        ide_chip;
    bool       symbiface_mouse; /* SYMBiFACE II / Cyboard PS/2 mouse present */
    Mouse      mouse;
    bool       m4;             /* M4 board hardware present */
    M4         m4_card;
    bool       symbnet;        /* 1984 emulator synthetic SymbOS network port */
    SymbNet    symbnet_card;
    bool       albireo;        /* Albireo / dual-CH376 card present       */
    bool       albireo_mouse;  /* Populate the second chip + accept HID   */
    CH376      ch376;          /* Left chip  @ 0xFE80/81 — storage, or HID
                                  mouse when albireo_mouse is true         */
    CH376      ch376_b;        /* Right chip @ 0xFE40/41 — storage in
                                  dual-chip mouse mode                     */
    Tape       tape;           /* cassette / .cdt image */
    /* Per-sample snapshot of the cassette data line, captured inside the
     * Z80 step loop at audio rate. Mixed into the PSG output frame so the
     * user hears the loading screeches. */
    s16        tape_audio[882];
    int        tape_audio_pos;
    int        tape_audio_cycles;

    /* Timing */
    int  cpu_clk_hz;      /* 4 MHz */
    int  cycles_per_frame;
    int  cycle_debt;      /* leftover cycles from previous frame */
    int  crtc_cycle_acc;  /* accumulated cycles for CRTC tick (4-cycle alignment) */

    /* Raster position (in character-clock units; 16 output pixels each) */
    int  raster_x;        /* 0 = first char after hsync end */
    int  raster_y;        /* 0 = first scanline after vsync */
    bool prev_hsync;
    bool prev_vsync;

    /* Pre-tick snapshot of CRTC state used by the per-char-clock render
     * loop. Lives in the struct (rather than as cpc_frame() locals) so
     * the Z80 bus tick hook can run a partial CRTC advance in the
     * middle of an IO instruction and the next chunk resumes from the
     * state the previous chunk left here. Updated after each CRTC tick. */
    u16  crtc_pre_ma;
    u8   crtc_pre_ra;
    bool crtc_pre_de;

    /* Cycles advanced via Z80Bus::tick mid-step; reset to 0 before each
     * z80_step. Subtracted from the instruction's total in cpc_frame()
     * so the post-IO chunk doesn't double-advance the bus. */
    int  bus_ticked_in_step;

    /* Debugger */
#define CPC_MAX_BREAKPOINTS 16
    bool paused;
    bool step_once;
    u16  breakpoints[CPC_MAX_BREAKPOINTS];
    bool bp_enabled[CPC_MAX_BREAKPOINTS];
} CPC;

extern int cpc_trace_io;      /* set to 1 to log CRTC/GA writes to stderr */
extern int cpc_trace_palette; /* set to 1 to log palette buffer state when B7F7=0xFF */
extern int m4_trace;          /* set to 1 to log M4 commands, responses, NMI state */
extern int cpc_trace_input;   /* set to 1 to log keyboard row 9 (joystick) scans */
extern int cpc_frame_count;   /* incremented by cpc_frame(); used by trace helpers */

/* Master debug enable, set from cfg->debug by main.c at startup. When 0
 * (default), dbg_getenv() returns NULL for every call, so every debug
 * site that uses it short-circuits. Non-debug env vars (ONE_K_CC_TABLES,
 * ONE_K_FAKE_RTC*, ONE_K_AUTOSTART_FRAMES, ONE_K_PASTE_GAP) continue to
 * call getenv() directly and are unaffected. */
extern int g_debug_enabled;
static inline const char *dbg_getenv(const char *name) {
    return g_debug_enabled ? getenv(name) : NULL;
}

/* Video capture (GIF89a) — owned by main.c. Returns true if recording
 * is active, regardless of how the call resolved. */
bool videocap_start(const char *path);   /* false on open failure */
void videocap_stop(void);
bool videocap_active(void);
int  videocap_frame_count(void);

int  cpc_init(CPC *cpc, CpcModel model, const char *rom_os, const char *rom_basic);
void cpc_reset(CPC *cpc);
void cpc_destroy(CPC *cpc);
void cpc_frame(CPC *cpc);        /* run one video frame (~20 ms) */
void cpc_key_event(CPC *cpc, SDL_Scancode sc, bool pressed);

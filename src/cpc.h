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
#include "amx.h"
#include "m4.h"
#include "symbnet.h"
#include "ch376.h"
#include "usifac.h"
#include "perryfi.h"
#include "tape.h"
#include "printer.h"

typedef enum { MODEL_464, MODEL_664, MODEL_6128 } CpcModel;

#define CPC_AUDIO_SAMPLE_RATE     44100
#define CPC_AUDIO_SAMPLES_FRAME   (CPC_AUDIO_SAMPLE_RATE / 50)
#define CPC_AUDIO_FRAME_CAPACITY  (CPC_AUDIO_SAMPLES_FRAME * 4)

typedef void (*CpcAudioSink)(void *userdata, const s16 *samples,
                             int frames, int sample_rate);

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
    bool       amx_mouse;      /* AMX mouse on the joystick port (Fallback Input) */
    Amx        amx;
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
    USIfAC     usifac;         /* USIfAC II RS232 serial @ 0xFBD0/D1       */
    Perryfi    perryfi;        /* PerryFi software AT-modem (hijacks USIfAC's
                                  data port when its present flag is set)   */
    Printer    printer;        /* Centronics @ 0xEFxx (Cairo → PDF host sink) */
    Tape       tape;           /* cassette / .cdt image */
    /* PSG + cassette audio generated inside the Z80 step loop at audio rate.
     * This preserves AY volume-register sample players: writes affect only
     * the samples after the corresponding CPU cycles have elapsed. */
    s16        audio_frame[CPC_AUDIO_FRAME_CAPACITY * 2];
    int        audio_frame_pos;
    int        audio_sample_cycles;
    CpcAudioSink audio_sink;
    void      *audio_sink_user;

    /* Timing */
    int  cpu_clk_hz;      /* 4 MHz */
    int  cycles_per_frame;
    int  cycle_debt;      /* leftover cycles from previous frame */
    int  crtc_cycle_acc;  /* accumulated cycles for CRTC tick (4-cycle alignment) */

    /* Render beam position. The CRTC generates sync pulses, but the monitor
     * free-runs between them; demos with R2/R3 rupture tricks depend on that
     * distinction. raster_y is the monitor scanline, while monitor_hpos is an
     * 8.8 fixed-point horizontal monitor position in CRTC character clocks. */
    int  raster_x;
    int  raster_y;
    int  monitor_vline;
    int  monitor_hpos;
    int  monitor_hsync;
    int  monitor_free_hsync;
    int  monitor_hsync_duration;
    int  monitor_min_hsync;
    int  monitor_max_hsync;
    int  monitor_hs_peak_pos;
    int  monitor_hs_start_pos;
    int  monitor_hs_end_pos;
    int  monitor_hs_peak_to_start;
    int  monitor_hs_start_to_peak;
    int  monitor_hs_end_to_peak;
    int  monitor_hs_peak_to_end;
    bool monitor_had_peak;
    bool monitor_in_hsync;
    bool monitor_frame_completed;
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
bool videocap_start(const char *path, int gif_width, int gif_fps,
                    bool gif_ffmpeg);    /* false on open failure */
void videocap_stop(void);
bool videocap_active(void);
int  videocap_frame_count(void);

bool audiocap_start(const char *path);
void audiocap_stop(void);
bool audiocap_active(void);
void audiocap_write(const s16 *samples, int frames, int sample_rate);

int  cpc_init(CPC *cpc, CpcModel model, const char *rom_os, const char *rom_basic, int scale);
void cpc_reset(CPC *cpc);
void cpc_destroy(CPC *cpc);
void cpc_set_audio_sink(CPC *cpc, CpcAudioSink sink, void *userdata);
/* Run until the monitor completes one video frame. Returns the number of
 * emulated CPU cycles consumed, or zero while paused. CRTC programs can make
 * a frame shorter or longer than the nominal 80,000 cycles, so frontends must
 * pace from this value rather than assuming every frame lasts exactly 20 ms. */
int cpc_frame(CPC *cpc);

/* Convert emulated CPU cycles to host nanoseconds. Invalid/paused values use
 * the nominal 50 Hz period so frontend event loops remain responsive. */
uint64_t cpc_cycles_to_ns(const CPC *cpc, int cycles);
void cpc_key_event(CPC *cpc, SDL_Scancode sc, bool pressed);

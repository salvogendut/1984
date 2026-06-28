#pragma once
#include "types.h"

/*
 * AY-3-8912 Programmable Sound Generator
 * The CPC uses it for audio and also for keyboard input (via the PSG's
 * I/O port A which reads the keyboard matrix).
 *
 * Output path: per-channel level tables built into L/R buses (Caprice32
 * ABC stereo layout — A mostly-left, B centre, C mostly-right), with a
 * configurable separation knob; per-bus one-pole DC blocker; one-pole
 * low-pass for square-wave de-harshening. Volume is a perceptual curve
 * baked into the level tables at config time.
 */

#define PSG_NUM_REGS 16

typedef struct {
    u8  reg[PSG_NUM_REGS];
    u8  selected;

    /* Per-channel tone counters */
    u16 tone_period[3];
    u16 tone_counter[3];
    u8  tone_output[3];

    /* Noise */
    u16 noise_period;
    u16 noise_counter;
    u32 noise_lfsr;

    /* Envelope */
    u16 env_period;
    u32 env_counter;
    u8  env_step;       /* 0-31: 32 steps per cycle */
    bool env_hold;
    bool env_dir;

    /* Fractional clock accumulator across samples */
    float clock_rem;

    /* Per-channel L/R amplitude tables (16 volume levels each), built
     * by psg_init / psg_set_volume / psg_set_stereo from the active
     * volume + stereo-separation settings. */
    int level_l[3][16];
    int level_r[3][16];

    /* DC-blocker state (one-pole high-pass), per output bus.
     * y[n] = x[n] - x[n-1] + R*y[n-1]   with R ≈ 0.995 */
    s32 hp_l_xprev, hp_l_yprev;
    s32 hp_r_xprev, hp_r_yprev;

    /* One-pole IIR low-pass state per bus */
    s32 lp_l_state, lp_r_state;

    /* Current settings (mirrored from Config so the audio thread doesn't
     * need to chase Config pointers). Apply via psg_set_*. */
    int  volume;        /* 0..100 — perceptual curve baked into tables */
    int  stereo_sep;    /* 0..255 — 0=mono (both buses identical),
                         *           255=full Caprice32 ABC separation */

    /* Keyboard: row data fed in externally */
    u8  kbd_data;
} PSG;

void psg_init(PSG *psg);
void psg_reset(PSG *psg);
void psg_select(PSG *psg, u8 reg);
void psg_write(PSG *psg, u8 val);
u8   psg_read(PSG *psg);
void psg_set_kbd_row(PSG *psg, u8 row_data);
void psg_load_registers(PSG *psg, const u8 regs[PSG_NUM_REGS], u8 selected,
                        bool has_env_state, u8 env_step, u8 env_direction);
void psg_store_registers(const PSG *psg, u8 regs[PSG_NUM_REGS], u8 *selected,
                         u8 *env_step, u8 *env_direction);

/* Rebuild the per-channel L/R amplitude tables. Call after changing
 * volume or stereo separation. Safe to call repeatedly. */
void psg_set_volume(PSG *psg, int vol_0_100);
void psg_set_stereo(PSG *psg, int sep_0_255);

/* Generate `frames` interleaved L,R s16 sample pairs into `buf`
 * (so buf must hold frames*2 s16 values) at the given clock rate. */
void psg_render_stereo(PSG *psg, s16 *buf, int frames,
                       int clock_hz, int sample_rate);

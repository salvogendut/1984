#pragma once
#include "types.h"

/*
 * AY-3-8912 Programmable Sound Generator
 * The CPC uses it for audio and also for keyboard input (via the PSG's
 * I/O port A which reads the keyboard matrix).
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

    /* One-pole IIR low-pass state (reduces square-wave aliasing harshness) */
    s32 lp_state;

    /* Keyboard: row data fed in externally */
    u8  kbd_data;
} PSG;

void psg_init(PSG *psg);
void psg_select(PSG *psg, u8 reg);
void psg_write(PSG *psg, u8 val);
u8   psg_read(PSG *psg);
void psg_set_kbd_row(PSG *psg, u8 row_data);

/* Generate `n` samples into `buf` (mono, 16-bit signed) at the given clock rate */
void psg_render(PSG *psg, s16 *buf, int n, int clock_hz, int sample_rate);

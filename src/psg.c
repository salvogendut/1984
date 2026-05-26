#include "psg.h"
#include <string.h>

/* AY-3-8912 amplitude table (c) Hacker KAY, scaled for 3-channel s16 output.
 * Source values 0-65535 divided by 6 → 3 channels sum to ≤ 32767. */
static const int vol_table[16] = {
       0,   139,   202,   295,   436,   645,   899,  1470,
    1732,  2784,  3889,  4882,  6161,  7736,  9199, 10922
};

void psg_init(PSG *psg) {
    memset(psg, 0, sizeof(*psg));
    psg->noise_lfsr = 1;
}

void psg_select(PSG *psg, u8 reg) {
    psg->selected = reg & 0x0F;
}

void psg_write(PSG *psg, u8 val) {
    psg->reg[psg->selected] = val;
    if (psg->selected == 13) {
        psg->env_step    = 0;
        psg->env_counter = 0;
        psg->env_hold    = false;
        psg->env_dir     = (val >> 2) & 1;  /* ATTACK bit: 1=up, 0=down */
    }
}

u8 psg_read(PSG *psg) {
    if (psg->selected == 14)
        return psg->kbd_data;
    return psg->reg[psg->selected];
}

void psg_set_kbd_row(PSG *psg, u8 row_data) {
    psg->kbd_data = row_data;
}

/* Run one AY clock tick and return the mixed level for all three channels. */
static int psg_tick(PSG *psg) {

    /* --- Tone counters (each half-period) --- */
    for (int c = 0; c < 3; c++) {
        u16 period = (u16)(((psg->reg[c*2+1] & 0x0F) << 8) | psg->reg[c*2]);
        if (!period) period = 1;
        if (++psg->tone_counter[c] >= period) {
            psg->tone_counter[c] = 0;
            psg->tone_output[c] ^= 1;
        }
    }

    /* --- Noise: step LFSR every noise_period*2 clocks --- */
    {
        u16 np = psg->reg[6] & 0x1F;
        if (!np) np = 1;
        if (++psg->noise_counter >= (u16)(np * 2)) {
            psg->noise_counter = 0;
            u32 bit = (psg->noise_lfsr ^ (psg->noise_lfsr >> 3)) & 1;
            psg->noise_lfsr = (psg->noise_lfsr >> 1) | (bit << 16);
        }
    }
    int noise_out = (int)(psg->noise_lfsr & 1);

    /* --- Envelope: 32 steps, one step every env_period clocks --- */
    if (!psg->env_hold) {
        u16 ep = (u16)((psg->reg[12] << 8) | psg->reg[11]);
        if (!ep) ep = 1;
        if (++psg->env_counter >= ep) {
            psg->env_counter = 0;
            psg->env_step++;
            if (psg->env_step >= 32) {
                u8   shape = psg->reg[13] & 0x0F;
                bool cont  = (shape >> 3) & 1;
                bool alt   = (shape >> 1) & 1;
                bool hold  = (shape >> 0) & 1;

                if (!cont) {
                    /* Single shot: hold at 0 */
                    psg->env_step = psg->env_dir ? 0 : 31;
                    psg->env_hold = true;
                } else if (hold) {
                    if (alt) psg->env_dir = !psg->env_dir;
                    psg->env_step = 31;
                    psg->env_hold = true;
                } else if (alt) {
                    /* Triangle: reverse direction */
                    psg->env_dir = !psg->env_dir;
                    psg->env_step = 0;
                } else {
                    /* Continuous sawtooth: restart */
                    psg->env_step = 0;
                }
            }
        }
    }
    /* 32 steps → 16 vol_table levels: map env_step/2 */
    u8 env_level = (u8)(psg->env_dir
                        ? (psg->env_step / 2)
                        : (u8)((31 - psg->env_step) / 2));

    /* --- Mix channels --- */
    int mix = 0;
    for (int c = 0; c < 3; c++) {
        bool tone_off  = (psg->reg[7] >> c)       & 1;
        bool noise_off = (psg->reg[7] >> (c + 3)) & 1;

        /* Disabled source contributes high (1) to the AND mixer */
        int tone_hi  = tone_off  ? 1 : (int)psg->tone_output[c];
        int noise_hi = noise_off ? 1 : noise_out;

        if (tone_hi && noise_hi) {
            u8 vr  = psg->reg[8 + c];
            u8 vol = (vr & 0x10) ? env_level : (vr & 0x0F);
            mix += vol_table[vol];
        }
    }

    return mix;
}

void psg_render(PSG *psg, s16 *buf, int n, int clock_hz, int sample_rate) {
    float clk_per_sample = (float)clock_hz / (float)sample_rate;

    for (int i = 0; i < n; i++) {
        /* Accumulate fractional clocks so pitch is exact */
        psg->clock_rem += clk_per_sample;
        int ticks = (int)psg->clock_rem;
        psg->clock_rem -= (float)ticks;
        if (ticks < 1) ticks = 1;

        /* Run one AY tick per clock and average → natural anti-aliasing */
        int mix_sum = 0;
        for (int t = 0; t < ticks; t++)
            mix_sum += psg_tick(psg);

        buf[i] = (s16)(mix_sum / ticks);
    }

    /* One-pole IIR low-pass: y[n] = (x[n] + y[n-1]) / 2  →  ~7 kHz cutoff at 44100 Hz.
     * Removes high-frequency aliasing that makes square waves sound harsh/metallic. */
    int lp = psg->lp_state;
    for (int i = 0; i < n; i++) {
        lp = ((int)buf[i] + lp) >> 1;
        buf[i] = (s16)lp;
    }
    psg->lp_state = lp;
}

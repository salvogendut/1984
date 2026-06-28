#include "psg.h"
#include <math.h>
#include <string.h>

/* AY-3-8912 amplitude table (c) Hacker KAY, scaled for one channel of
 * s16 stereo output. Source values 0-65535; we keep them in range so
 * three channels summed and panned can't exceed s16 range after the
 * stereo + volume preamp is applied. */
static const int ay_amp[16] = {
       0,   139,   202,   295,   436,   645,   899,  1470,
    1732,  2784,  3889,  4882,  6161,  7736,  9199, 10922
};

/* Per-channel pan factors at full stereo separation (sep=255).
 * Layout matches Caprice32's Index_AL/AR/BL/BR/CL/CR (255/13/170/170/13/255
 * out of 255) — A mostly-left, B centre, C mostly-right. */
static const float pan_full[3][2] = {
    { 255.0f / 255.0f,  13.0f / 255.0f },   /* A: L=full, R=trickle */
    { 170.0f / 255.0f, 170.0f / 255.0f },   /* B: equal both sides   */
    {  13.0f / 255.0f, 255.0f / 255.0f },   /* C: L=trickle, R=full  */
};

/* DC-blocker pole — closer to 1.0 = lower corner frequency. 0.995 at
 * 44.1 kHz gives a ~35 Hz corner: kills the DC bump without touching
 * audible bass. Stored as fixed-point Q15 so the inner loop is integer. */
#define HP_R_Q15  32604   /* round(0.995 * 32768) */

static void rebuild_levels(PSG *psg) {
    /* Perceptual volume curve: out = (in/100)^2 — gentle on small moves,
     * fast taper near max. Caprice32 uses exp(vol*ln2/PreAmpMax)-1; the
     * shape is similar at modest preamp settings and (in)^2 is cheaper. */
    int   v   = psg->volume; if (v < 0) v = 0; else if (v > 100) v = 100;
    float pre = (float)v * (float)v / 10000.0f;

    int sep = psg->stereo_sep; if (sep < 0) sep = 0; else if (sep > 255) sep = 255;
    float s = (float)sep / 255.0f;

    for (int c = 0; c < 3; c++) {
        /* Lerp each pan factor between 1.0 (mono — both buses equal)
         * and the Caprice32 full-separation value. */
        float pL = 1.0f + (pan_full[c][0] - 1.0f) * s;
        float pR = 1.0f + (pan_full[c][1] - 1.0f) * s;
        for (int i = 0; i < 16; i++) {
            psg->level_l[c][i] = (int)((float)ay_amp[i] * pL * pre);
            psg->level_r[c][i] = (int)((float)ay_amp[i] * pR * pre);
        }
    }
}

void psg_init(PSG *psg) {
    memset(psg, 0, sizeof(*psg));
    psg->noise_lfsr = 1;
    psg->volume     = 80;
    psg->stereo_sep = 255;
    rebuild_levels(psg);
}

void psg_reset(PSG *psg) {
    /* Clear AY state but preserve user audio settings (volume / stereo
     * separation, level tables, DC-blocker + low-pass state) across a
     * warm reset. */
    memset(psg->reg, 0, sizeof(psg->reg));
    psg->selected = 0;
    for (int c = 0; c < 3; c++) {
        psg->tone_period[c]  = 0;
        psg->tone_counter[c] = 0;
        psg->tone_output[c]  = 0;
    }
    psg->noise_period  = 0;
    psg->noise_counter = 0;
    psg->noise_lfsr    = 1;
    psg->env_period    = 0;
    psg->env_counter   = 0;
    psg->env_step      = 0;
    psg->env_hold      = false;
    psg->env_dir       = false;
    psg->clock_rem     = 0.0f;
    psg->kbd_data      = 0;
}

void psg_set_volume(PSG *psg, int vol_0_100) {
    psg->volume = vol_0_100;
    rebuild_levels(psg);
}

void psg_set_stereo(PSG *psg, int sep_0_255) {
    psg->stereo_sep = sep_0_255;
    rebuild_levels(psg);
}

static u8 psg_register_mask(u8 reg, u8 val) {
    switch (reg) {
    case 1:
    case 3:
    case 5:
    case 13:
        return val & 0x0F;
    case 6:
    case 8:
    case 9:
    case 10:
        return val & 0x1F;
    case 7:
        return val & 0x3F;
    default:
        return val;
    }
}

void psg_select(PSG *psg, u8 reg) {
    psg->selected = reg;
}

void psg_write(PSG *psg, u8 val) {
    if (psg->selected >= PSG_NUM_REGS)
        return;
    val = psg_register_mask(psg->selected, val);
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
    if (psg->selected >= PSG_NUM_REGS)
        return 0xFF;
    return psg->reg[psg->selected];
}

void psg_set_kbd_row(PSG *psg, u8 row_data) {
    psg->kbd_data = row_data;
}

void psg_load_registers(PSG *psg, const u8 regs[PSG_NUM_REGS], u8 selected,
                        bool has_env_state, u8 env_step, u8 env_direction) {
    u8 kbd_data = psg->kbd_data;

    memset(psg->reg, 0, sizeof(psg->reg));
    for (u8 i = 0; i < PSG_NUM_REGS; i++)
        psg->reg[i] = psg_register_mask(i, regs[i]);

    psg->selected = selected;
    memset(psg->tone_counter, 0, sizeof(psg->tone_counter));
    memset(psg->tone_output, 0, sizeof(psg->tone_output));
    psg->noise_counter = 0;
    psg->noise_lfsr = 1;
    psg->env_counter = 0;
    psg->clock_rem = 0.0f;
    psg->hp_l_xprev = psg->hp_l_yprev = 0;
    psg->hp_r_xprev = psg->hp_r_yprev = 0;
    psg->lp_l_state = psg->lp_r_state = 0;
    psg->kbd_data = kbd_data;

    if (has_env_state) {
        u8 level = (u8)(env_step & 0x0F);
        if (env_direction == 0x01) {
            psg->env_dir = true;
            psg->env_step = (u8)(level * 2);
            psg->env_hold = false;
        } else if (env_direction == 0xFF) {
            psg->env_dir = false;
            psg->env_step = (u8)(31 - (level * 2));
            psg->env_hold = false;
        } else {
            psg->env_dir = true;
            psg->env_step = (u8)(level * 2);
            psg->env_hold = true;
        }
    } else {
        psg->env_step = 0;
        psg->env_hold = false;
        psg->env_dir = (psg->reg[13] >> 2) & 1;
    }
}

void psg_store_registers(const PSG *psg, u8 regs[PSG_NUM_REGS], u8 *selected,
                         u8 *env_step, u8 *env_direction) {
    memcpy(regs, psg->reg, PSG_NUM_REGS);
    if (selected)
        *selected = psg->selected;
    if (env_step) {
        *env_step = (u8)(psg->env_dir
                         ? (psg->env_step / 2)
                         : ((31 - psg->env_step) / 2));
    }
    if (env_direction)
        *env_direction = psg->env_hold ? 0x00 : (psg->env_dir ? 0x01 : 0xFF);
}

/* Run one AY clock tick. Returns the per-channel amplitude index
 * (0..15) into out_amp[3]; a channel whose tone-and-noise mixer
 * gates to silent contributes 0. */
static void psg_tick(PSG *psg, int out_amp[3]) {

    /* --- Tone counters --- */
    /* AY has an internal ÷8 prescaler before the tone counters; multiply
     * register period by 8 so f = 1MHz / (16 × N) — standard AY formula. */
    for (int c = 0; c < 3; c++) {
        u16 period = (u16)(((psg->reg[c*2+1] & 0x0F) << 8) | psg->reg[c*2]);
        if (!period) period = 1;
        if (++psg->tone_counter[c] >= (u16)(period * 8)) {
            psg->tone_counter[c] = 0;
            psg->tone_output[c] ^= 1;
        }
    }

    /* --- Noise: step LFSR every noise_period*16 clocks (same prescaler) --- */
    {
        u16 np = psg->reg[6] & 0x1F;
        if (!np) np = 1;
        if (++psg->noise_counter >= (u16)(np * 16)) {
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
        if (++psg->env_counter >= (u32)ep * 8) {
            psg->env_counter = 0;
            psg->env_step++;
            if (psg->env_step >= 32) {
                u8   shape = psg->reg[13] & 0x0F;
                bool cont  = (shape >> 3) & 1;
                bool alt   = (shape >> 1) & 1;
                bool hold  = (shape >> 0) & 1;

                if (!cont) {
                    psg->env_step = psg->env_dir ? 0 : 31;
                    psg->env_hold = true;
                } else if (hold) {
                    if (alt) psg->env_dir = !psg->env_dir;
                    psg->env_step = 31;
                    psg->env_hold = true;
                } else if (alt) {
                    psg->env_dir = !psg->env_dir;
                    psg->env_step = 0;
                } else {
                    psg->env_step = 0;
                }
            }
        }
    }
    u8 env_level = (u8)(psg->env_dir
                        ? (psg->env_step / 2)
                        : (u8)((31 - psg->env_step) / 2));

    /* --- Per-channel mixer: produce a 0..15 amplitude index per channel. --- */
    for (int c = 0; c < 3; c++) {
        bool tone_off  = (psg->reg[7] >> c)       & 1;
        bool noise_off = (psg->reg[7] >> (c + 3)) & 1;

        if (tone_off && noise_off) {
            /* Channel is gated off: AY holds the level high, but with
             * the DC blocker downstream that becomes silence — emit 0. */
            out_amp[c] = 0;
            continue;
        }

        /* Disabled source contributes high (1) to the AND mixer */
        int tone_hi  = tone_off  ? 1 : (int)psg->tone_output[c];
        int noise_hi = noise_off ? 1 : noise_out;

        u8 vr  = psg->reg[8 + c];
        u8 vol = (vr & 0x10) ? env_level : (vr & 0x0F);

        out_amp[c] = (tone_hi && noise_hi) ? (int)vol : 0;
    }
}

/* One-pole DC blocker: y = x - x_prev + R*y_prev (R in Q15).
 * Pure integer; sample range fits comfortably in s32. */
static inline s32 hp_step(s32 x, s32 *xprev, s32 *yprev) {
    s32 y = (s32)(x - *xprev + (s32)(((int64_t)HP_R_Q15 * (int64_t)(*yprev)) >> 15));
    *xprev = x;
    *yprev = y;
    return y;
}

static inline s16 sat16(s32 v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (s16)v;
}

void psg_render_stereo(PSG *psg, s16 *buf, int frames,
                       int clock_hz, int sample_rate) {
    float clk_per_sample = (float)clock_hz / (float)sample_rate;

    for (int i = 0; i < frames; i++) {
        psg->clock_rem += clk_per_sample;
        int ticks = (int)psg->clock_rem;
        psg->clock_rem -= (float)ticks;
        if (ticks < 1) ticks = 1;

        /* Average across the AY ticks that fall in this output sample.
         * Cheap anti-aliasing — same as the original mono path. */
        s32 lsum = 0, rsum = 0;
        for (int t = 0; t < ticks; t++) {
            int amp[3];
            psg_tick(psg, amp);
            lsum += psg->level_l[0][amp[0]]
                  + psg->level_l[1][amp[1]]
                  + psg->level_l[2][amp[2]];
            rsum += psg->level_r[0][amp[0]]
                  + psg->level_r[1][amp[1]]
                  + psg->level_r[2][amp[2]];
        }
        s32 lraw = lsum / ticks;
        s32 rraw = rsum / ticks;

        /* DC blocker (replaces the old (mix*2 - active_scale) trick;
         * no clicks when reg7 enables/disables a channel mid-frame). */
        s32 lhp = hp_step(lraw, &psg->hp_l_xprev, &psg->hp_l_yprev);
        s32 rhp = hp_step(rraw, &psg->hp_r_xprev, &psg->hp_r_yprev);

        /* Original one-pole IIR low-pass — keeps square-wave harshness
         * down. y[n] = (x[n] + y[n-1]) / 2 — corner near sample_rate/(2π). */
        psg->lp_l_state = (lhp + psg->lp_l_state) >> 1;
        psg->lp_r_state = (rhp + psg->lp_r_state) >> 1;

        buf[i*2  ] = sat16(psg->lp_l_state);
        buf[i*2+1] = sat16(psg->lp_r_state);
    }
}

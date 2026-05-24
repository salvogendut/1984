#include "psg.h"
#include <string.h>

void psg_init(PSG *psg) {
    memset(psg, 0, sizeof(*psg));
    psg->noise_lfsr = 1;
}

void psg_select(PSG *psg, u8 reg) {
    psg->selected = reg & 0x0F;
}

void psg_write(PSG *psg, u8 val) {
    psg->reg[psg->selected] = val;
}

u8 psg_read(PSG *psg) {
    if (psg->selected == 15)   /* I/O port B = keyboard columns */
        return psg->kbd_data;
    return psg->reg[psg->selected];
}

void psg_set_kbd_row(PSG *psg, u8 row_data) {
    psg->kbd_data = row_data;
}

void psg_render(PSG *psg, s16 *buf, int n, int clock_hz, int sample_rate) {
    /* Clocks per sample */
    int clk_per_sample = clock_hz / sample_rate;

    for (int i = 0; i < n; i++) {
        int mix = 0;

        for (int c = 0; c < 3; c++) {
            u16 period = ((psg->reg[c*2+1] & 0x0F) << 8) | psg->reg[c*2];
            if (!period) period = 1;

            psg->tone_counter[c] += clk_per_sample;
            while (psg->tone_counter[c] >= period) {
                psg->tone_counter[c] -= period;
                psg->tone_output[c] ^= 1;
            }

            bool tone_off  = (psg->reg[7] >> c) & 1;
            bool noise_off = (psg->reg[7] >> (c + 3)) & 1;

            u8 vol = psg->reg[8 + c] & 0x0F;
            int level = vol << 11;

            if (tone_off && noise_off) {
                mix += level;
            } else {
                if (!tone_off && psg->tone_output[c]) mix += level;
            }
        }

        buf[i] = (s16)(mix / 3);
    }
}

#include "crtc.h"
#include <string.h>

void crtc_init(CRTC *c) {
    memset(c, 0, sizeof(*c));
    c->reg[0]  = 63;    /* H Total */
    c->reg[1]  = 40;    /* H Displayed */
    c->reg[2]  = 46;    /* H Sync Position */
    c->reg[3]  = 0x8E;  /* H/V Sync Widths: H=14, V=8 */
    c->reg[4]  = 38;    /* V Total */
    c->reg[5]  = 0;     /* V Total Adjust */
    c->reg[6]  = 25;    /* V Displayed */
    c->reg[7]  = 30;    /* V Sync Position */
    c->reg[9]  = 7;     /* Max Raster */
    c->reg[12] = 0x30;  /* Display Start High */
    c->reg[13] = 0x00;  /* Display Start Low */

    c->ma_row_start = ((u16)(c->reg[12] << 8) | c->reg[13]) & 0x3FFF;
    c->ma_next_row = c->ma_row_start;
    c->ma = c->ma_row_start;
    c->h_display = true;
    c->v_display = true;
    c->display_enable = true;
}

void crtc_select(CRTC *c, u8 reg) { c->selected = reg & 0x1F; }

void crtc_write(CRTC *c, u8 val) {
    if (c->selected < CRTC_NUM_REGS) {
        c->reg[c->selected] = val;
        /* Display timing comparators are edge-triggered. A write that
         * matches the current vertical row can turn display off immediately,
         * but moving R6 beyond the beam does not re-enable it this frame. */
        if (c->selected == 6 && c->vcc == c->reg[6])
            c->v_display = false;
        c->display_enable = c->h_display && c->v_display;
    }
}

u8 crtc_read(CRTC *c) {
    return (c->selected >= 12 && c->selected <= 17) ? c->reg[c->selected] : 0;
}

u8 crtc_read_status(CRTC *c) {
    /* Type 1/3/4 status register: bit 6 = LPEN strobe (unimplemented),
     * bit 5 = VSYNC active. Demos poll this to time effects without the
     * 2-line GA→PPI VSYNC delay. */
    return (c->vsync ? 0x20 : 0x00);
}

void crtc_tick(CRTC *c) {
    c->mode_latch = false;

    /* Horizontal counter advances; MA tracks along the row */
    c->hcc++;
    c->ma++;

    /* --- HSYNC --- */
    u16 hsw = c->reg[3] & 0x0F;
    if (!hsw) hsw = 16;
    if (!c->hsync && c->hcc == c->reg[2]) {
        c->hsync = true;
        c->hsc = 0;
        c->mode_latched_this_hsync = false;
    }
    if (c->hsync) {
        if (c->hsc == hsw) {
            c->hsync = false;
            if (!c->mode_latched_this_hsync) {
                c->mode_latch = true;
                c->mode_latched_this_hsync = true;
            }
        } else {
            c->hsc++;
            if (hsw < 16)
                c->hsc &= 0x0F;
            /* The Gate Array stops observing a long CRTC HSYNC after seven
             * character clocks and latches the requested mode at that point. */
            if (c->hsc == 7 && !c->mode_latched_this_hsync) {
                c->mode_latch = true;
                c->mode_latched_this_hsync = true;
            }
        }
    }

    bool end_of_line = c->hcc > c->reg[0];

    /* On the final raster of a character row, R1 captures the address for
     * the next row. If software moves R1 behind the beam, the comparator is
     * missed and the current row address is repeated, as on the real CRTC. */
    if (c->vlc == c->reg[9] &&
        (c->hcc == c->reg[1] || (end_of_line && c->reg[1] == 0)))
        c->ma_next_row = (u16)((c->ma_row_start + c->reg[1]) & 0x3FFF);

    /* --- End of line --- */
    if (end_of_line) {
        c->hcc = 0;
        c->h_display = c->reg[1] != 0;

        /* VSYNC line counter (counts in scan lines = HSYNCs) */
        if (c->vsync) {
            c->vsc++;
            if (c->vsc >= 16)   /* hardware-fixed 16 scan lines on CPC */
                c->vsync = false;
        }

        if (c->in_vadjust) {
            /* Vertical-adjust period (R5 raster lines after the last char row).
             * Per ACCC §6.1.1: when C5 reaches R5, the frame restarts with
             * C4=C5=C9=0 and MA reloaded from R12/R13. */
            c->vac++;
            if (c->vac > c->reg[5]) {
                c->in_vadjust = false;
                c->vac = 0;
                c->vcc = 0;
                c->vlc = 0;
                c->ma_row_start = ((u16)(c->reg[12] << 8) | c->reg[13]) & 0x3FFF;
                c->ma_next_row = c->ma_row_start;
                c->v_display = c->reg[6] != 0;
            }
        } else {
            /* Raster line within character row */
            c->vlc++;
            if (c->vlc > c->reg[9]) {
                /* New character row: advance row start address */
                c->vlc = 0;
                c->vcc++;
                if (c->vcc == c->reg[6])
                    c->v_display = false;

                /* V total reached: either enter R5 vertical-adjust or
                 * restart the frame immediately if R5 is 0. */
                if (c->vcc > c->reg[4]) {
                    if (c->reg[5]) {
                        c->in_vadjust = true;
                        c->vac = 0;
                    } else {
                        c->vcc = 0;
                        c->vlc = 0;
                        c->ma_row_start = ((u16)(c->reg[12] << 8) | c->reg[13]) & 0x3FFF;
                        c->ma_next_row = c->ma_row_start;
                        c->v_display = c->reg[6] != 0;
                    }
                }
            }
        }

        /* The address pipeline advances every scan line. On non-final
         * rasters ma_next_row still holds the current row start. */
        c->ma_row_start = c->ma_next_row;
        c->ma = c->ma_row_start;
    } else if (c->hcc == c->reg[1]) {
        c->h_display = false;
    }

    /* Continuous R7 comparator: real CRTC starts VSYNC the moment C4
     * matches R7, not only at the next row-boundary. Demos that re-time
     * VSYNC by writing R7 mid-frame (HBL effects, second-VSYNC tricks)
     * depend on this. */
    if (c->vcc == c->reg[7] && !c->vsync) {
        c->vsync = true;
        c->vsc = 0;
    }

    c->display_enable = c->h_display && c->v_display;
}

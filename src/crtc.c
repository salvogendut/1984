#include "crtc.h"
#include <string.h>

static u8 crtc_masked_reg_value(const CRTC *c, u8 reg, u8 val) {
    switch (reg) {
    case 4:
    case 6:
    case 7:
    case 10:
        return val & 0x7F;
    case 5:
    case 9:
    case 11:
        return val & 0x1F;
    case 8:
        return (c->type == CRTC_TYPE_1 || c->type == CRTC_TYPE_2) ? (val & 0x03) : val;
    case 12:
    case 14:
        return val & 0x3F;
    default:
        return val;
    }
}

static u16 crtc_hsync_width(const CRTC *c) {
    u16 hsw = c->reg[3] & 0x0F;
    if (!hsw && (c->type == CRTC_TYPE_2 || c->type == CRTC_TYPE_3))
        hsw = 16;
    return hsw;
}

static u16 crtc_vsync_width(const CRTC *c) {
    if (c->type == CRTC_TYPE_0 || c->type == CRTC_TYPE_3) {
        u16 vsw = c->reg[3] >> 4;
        return vsw ? vsw : 16;
    }
    return 16;
}

static u16 crtc_start_addr(const CRTC *c) {
    return ((u16)(c->reg[12] << 8) | c->reg[13]) & 0x3FFF;
}

static void crtc_latch_line_state(CRTC *c) {
    c->line_last_raster = c->vlc == c->reg[9];
    c->line_last_frame = c->line_last_raster && c->vcc == c->reg[4];
}

void crtc_init(CRTC *c) {
    memset(c, 0, sizeof(*c));
    c->type = CRTC_TYPE_1;
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

    c->ma_row_start = crtc_start_addr(c);
    c->ma_next_row = c->ma_row_start;
    c->ma = c->ma_row_start;
    c->h_display = true;
    c->v_display = true;
    c->display_enable = true;
    crtc_latch_line_state(c);
}

void crtc_set_type(CRTC *c, CrtcType type) {
    if (type < CRTC_TYPE_0 || type > CRTC_TYPE_3)
        type = CRTC_TYPE_0;
    c->type = type;
    c->reg[8] = crtc_masked_reg_value(c, 8, c->reg[8]);
}

void crtc_recompute_state(CRTC *c) {
    crtc_latch_line_state(c);
    c->h_display = c->reg[1] != 0 && c->hcc < c->reg[1];
    c->v_display = c->reg[6] != 0 && c->vcc < c->reg[6];
    c->display_enable = c->h_display && c->v_display;
    c->new_scanline = false;
    c->mode_latch = false;
}

void crtc_select(CRTC *c, u8 reg) { c->selected = reg & 0x1F; }

void crtc_write(CRTC *c, u8 val) {
    if (c->selected < 16) {
        c->reg[c->selected] = crtc_masked_reg_value(c, c->selected, val);
        /* Display timing comparators are edge-triggered. A write that
         * matches the current vertical row can turn display off immediately,
         * but moving R6 beyond the beam does not re-enable it this frame. */
        if (c->selected == 6 && c->vcc == c->reg[6])
            c->v_display = false;
        /* C9/R9 and C4/R4 are sampled for the current scanline while C0 is
         * still in its first two character positions. Later writes affect
         * following lines, not this line's row/frame reset decision. */
        if ((c->selected == 4 || c->selected == 9) && c->hcc <= 1)
            crtc_latch_line_state(c);
        c->display_enable = c->h_display && c->v_display;
    }
}

u8 crtc_read(CRTC *c) {
    u8 reg = c->selected;
    switch (c->type) {
    case CRTC_TYPE_0:
    case CRTC_TYPE_3:
        return (reg >= 12 && reg <= 17) ? c->reg[reg] : 0;
    case CRTC_TYPE_1:
        if (reg >= 14 && reg <= 17) return c->reg[reg];
        if (reg == 31) return 0xFF;
        return 0;
    case CRTC_TYPE_2:
    default:
        return (reg >= 14 && reg <= 17) ? c->reg[reg] : 0;
    }
}

u8 crtc_read_status(CRTC *c) {
    if (c->type == CRTC_TYPE_1)
        return (c->vcc >= c->reg[6]) ? 0x20 : 0x00; /* bit 5 = vertical blanking */
    if (c->type == CRTC_TYPE_3)
        return crtc_read(c);
    return 0xFF;
}

void crtc_tick(CRTC *c) {
    c->new_scanline = false;
    c->mode_latch = false;

    u8 old_hcc = (u8)c->hcc;

    /* Horizontal counter advances; MA tracks along the row */
    c->hcc = (u8)((c->hcc + 1) & 0xFF);
    c->ma++;

    /* --- HSYNC --- */
    u16 hsw = crtc_hsync_width(c);
    if (hsw && !c->hsync && c->hcc == c->reg[2]) {
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

    bool end_of_line = old_hcc == c->reg[0];

    /* On the final raster of a character row, R1 captures the address for
     * the next row. If software moves R1 behind the beam, the comparator is
     * missed and the current row address is repeated, as on the real CRTC. */
    if (c->line_last_raster &&
        (c->hcc == c->reg[1] || (end_of_line && c->reg[1] == 0))) {
        c->ma_next_row = (end_of_line && c->reg[1] == 0)
            ? c->ma_row_start
            : (u16)(c->ma & 0x3FFF);
    }

    bool end_char_row = c->line_last_raster;
    bool end_frame_line = c->line_last_frame;

    /* --- End of line --- */
    if (end_of_line) {
        c->new_scanline = true;
        c->hcc = 0;
        c->h_display = c->reg[1] != 0;

        /* VSYNC line counter (counts in scan lines = HSYNCs) */
        if (c->vsync) {
            c->vsc++;
            if (c->vsc >= crtc_vsync_width(c))
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
                c->ma_row_start = crtc_start_addr(c);
                c->ma_next_row = c->ma_row_start;
                c->v_display = c->reg[6] != 0;
            }
        } else if (end_char_row) {
            /* Raster line within character row */
            c->vlc = 0;

            /* Counter registers are comparators, not "greater than" limits.
             * If software lowers R4/R9 after the internal counter has already
             * passed the new value, the CRTC keeps counting until the counter
             * wraps and the comparator can match again. Line-by-line rupture
             * effects depend on that overflow behaviour. */
            if (end_frame_line) {
                if (c->reg[5]) {
                    c->in_vadjust = true;
                    c->vac = 0;
                } else {
                    c->vcc = 0;
                    c->ma_row_start = crtc_start_addr(c);
                    c->ma_next_row = c->ma_row_start;
                    c->v_display = c->reg[6] != 0;
                }
            } else {
                c->vcc = (c->vcc + 1) & 0x7F;
                if (c->vcc == c->reg[6])
                    c->v_display = false;
            }
        } else {
            c->vlc = (c->vlc + 1) & 0x1F;
        }

        /* The UM6845R re-reads R12/R13 at the start of every scanline while
         * VCC is zero. A write changes the register immediately, but must not
         * splice a new address into the scanline currently being output. */
        if (c->type == CRTC_TYPE_1 && c->vcc == 0) {
            c->ma_row_start = crtc_start_addr(c);
            c->ma_next_row = c->ma_row_start;
        }

        /* The address pipeline advances every scan line. On non-final
         * rasters ma_next_row still holds the current row start. */
        c->ma_row_start = c->ma_next_row;
        c->ma = c->ma_row_start;
        crtc_latch_line_state(c);
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

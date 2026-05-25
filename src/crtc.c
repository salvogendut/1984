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
    c->ma = c->ma_row_start;
    c->display_enable = true;
}

void crtc_select(CRTC *c, u8 reg) { c->selected = reg & 0x1F; }

void crtc_write(CRTC *c, u8 val) {
    if (c->selected < CRTC_NUM_REGS)
        c->reg[c->selected] = val;
}

u8 crtc_read(CRTC *c) {
    return (c->selected >= 12 && c->selected <= 17) ? c->reg[c->selected] : 0;
}

void crtc_tick(CRTC *c) {
    /* Horizontal counter advances; MA tracks along the row */
    c->hcc++;
    c->ma++;

    /* --- HSYNC --- */
    if (c->hcc == c->reg[2]) {
        c->hsync = true;
        c->hsc = 0;
    }
    if (c->hsync) {
        c->hsc++;
        u8 hsw = c->reg[3] & 0x0F;
        if (!hsw) hsw = 16;
        if (c->hsc >= hsw)
            c->hsync = false;
    }

    /* --- End of line --- */
    if (c->hcc > c->reg[0]) {
        c->hcc = 0;

        /* VSYNC line counter (counts in scan lines = HSYNCs) */
        if (c->vsync) {
            c->vsc++;
            if (c->vsc >= 16)   /* hardware-fixed 16 scan lines on CPC */
                c->vsync = false;
        }

        /* Raster line within character row */
        c->vlc++;
        if (c->vlc > c->reg[9]) {
            /* New character row: advance row start address */
            c->vlc = 0;
            c->vcc++;
            c->ma_row_start += c->reg[1];

            /* VSYNC trigger */
            if (c->vcc == c->reg[7] && !c->vsync) {
                c->vsync = true;
                c->vsc = 0;
            }

            /* V total: new frame */
            if (c->vcc > c->reg[4]) {
                c->vcc = 0;
                c->vlc = 0;
                c->ma_row_start = ((u16)(c->reg[12] << 8) | c->reg[13]) & 0x3FFF;
            }
        }

        /* Reload MA to the row start for the next scan line */
        c->ma = c->ma_row_start;
    }

    c->display_enable = (c->hcc < c->reg[1]) && (c->vcc < c->reg[6]);
}

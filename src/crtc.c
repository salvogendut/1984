#include "crtc.h"
#include <string.h>

void crtc_init(CRTC *c) {
    memset(c, 0, sizeof(*c));
    /* Default register values typical of CPC startup */
    c->reg[0]  = 63;   /* H Total */
    c->reg[1]  = 40;   /* H Displayed */
    c->reg[2]  = 46;   /* H Sync Position */
    c->reg[3]  = 0x8E; /* H/V Sync Widths */
    c->reg[4]  = 38;   /* V Total */
    c->reg[5]  = 0;    /* V Total Adjust */
    c->reg[6]  = 25;   /* V Displayed */
    c->reg[7]  = 30;   /* V Sync Position */
    c->reg[9]  = 7;    /* Max Scan Line */
    c->reg[12] = 0x30; /* Start Address High */
    c->reg[13] = 0x00; /* Start Address Low */
    c->display_enable = true;
}

void crtc_select(CRTC *c, u8 reg) {
    c->selected = reg & 0x1F;
}

void crtc_write(CRTC *c, u8 val) {
    if (c->selected < CRTC_NUM_REGS)
        c->reg[c->selected] = val;
}

u8 crtc_read(CRTC *c) {
    if (c->selected >= 12 && c->selected <= 17)
        return c->reg[c->selected];
    return 0;
}

void crtc_tick(CRTC *c) {
    c->ma++;

    /* Horizontal */
    c->hcc++;

    /* Horizontal sync */
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

    if (c->hcc > c->reg[0]) {
        c->hcc = 0;

        /* Vertical sync */
        if (c->vcc == c->reg[7]) {
            c->vsync = true;
            c->vsc = 0;
        }
        if (c->vsync) {
            c->vsc++;
            u8 vsw = (c->reg[3] >> 4) & 0x0F;
            if (!vsw) vsw = 16;
            if (c->vsc >= vsw)
                c->vsync = false;
        }

        c->vlc++;
        if (c->vlc > c->reg[9]) {
            c->vlc = 0;
            c->vcc++;

            if (c->vcc > c->reg[4]) {
                c->vcc = 0;
                c->ma = ((c->reg[12] << 8) | c->reg[13]) & 0x3FFF;
                c->ma_row_start = c->ma;
            }
        }
        c->ma_row_start += c->reg[1];
        c->ma = c->ma_row_start;
    }

    c->display_enable = (c->hcc < c->reg[1]) && (c->vcc < c->reg[6]);
}

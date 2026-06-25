#pragma once
#include "types.h"

/*
 * Motorola 6845 CRTC — video timing generator.
 * The CPC uses it to drive the Gate Array's scan sequencer.
 *
 * Only registers actually used by CPC software are modelled initially.
 */

#define CRTC_NUM_REGS 18

typedef struct {
    u8  reg[CRTC_NUM_REGS];
    u8  selected;          /* address register */

    /* Internal counters */
    u16 hcc;               /* C0: horizontal character counter */
    u16 vcc;               /* C4: vertical character counter */
    u16 vlc;               /* C9: vertical line counter (scan line within char row) */
    u16 vsc;               /* C3h: vertical sync counter */
    u16 hsc;               /* C3l: horizontal sync counter */
    u16 vac;               /* C5: vertical-adjust raster counter (R5) */
    bool in_vadjust;       /* true while we're in the R5 vertical-adjust period */

    u16 ma;                /* memory address */
    u16 ma_row_start;

    bool hsync;
    bool vsync;
    bool h_display;
    bool v_display;
    bool display_enable;
    bool mode_latch;       /* one-tick GA mode-latch event */
    bool mode_latched_this_hsync;
} CRTC;

void crtc_init(CRTC *c);
void crtc_select(CRTC *c, u8 reg);
void crtc_write(CRTC *c, u8 val);
u8   crtc_read(CRTC *c);
u8   crtc_read_status(CRTC *c);     /* &BE00 — type 1/3/4 status register */
void crtc_tick(CRTC *c);       /* one character clock (1 MHz) */

/* Derived helpers */
static inline u16 crtc_screen_addr(CRTC *c) { return c->ma; }
static inline bool crtc_hsync(CRTC *c)      { return c->hsync; }
static inline bool crtc_vsync(CRTC *c)      { return c->vsync; }
static inline bool crtc_de(CRTC *c)         { return c->display_enable; }
static inline bool crtc_mode_latch(CRTC *c) { return c->mode_latch; }

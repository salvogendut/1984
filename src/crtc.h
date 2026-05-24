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
    u16 hcc;               /* horizontal character counter */
    u16 vcc;               /* vertical character counter */
    u16 vlc;               /* vertical line counter (scan line within char row) */
    u16 vsc;               /* vertical sync counter */
    u16 hsc;               /* horizontal sync counter */

    u16 ma;                /* memory address */
    u16 ma_row_start;

    bool hsync;
    bool vsync;
    bool display_enable;
} CRTC;

void crtc_init(CRTC *c);
void crtc_select(CRTC *c, u8 reg);
void crtc_write(CRTC *c, u8 val);
u8   crtc_read(CRTC *c);
void crtc_tick(CRTC *c);       /* one character clock (1 MHz) */

/* Derived helpers */
static inline u16 crtc_screen_addr(CRTC *c) { return c->ma; }
static inline bool crtc_hsync(CRTC *c)      { return c->hsync; }
static inline bool crtc_vsync(CRTC *c)      { return c->vsync; }
static inline bool crtc_de(CRTC *c)         { return c->display_enable; }

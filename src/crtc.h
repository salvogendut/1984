#pragma once
#include "types.h"

/*
 * Motorola 6845 CRTC — video timing generator.
 * The CPC uses it to drive the Gate Array's scan sequencer.
 *
 * Only registers actually used by CPC software are modelled initially.
 */

#define CRTC_NUM_REGS 18

typedef enum {
    CRTC_TYPE_0 = 0,   /* HD6845S */
    CRTC_TYPE_1 = 1,   /* UM6845R */
    CRTC_TYPE_2 = 2,   /* MC6845 */
    CRTC_TYPE_3 = 3,   /* AMS40489 / Plus ASIC */
} CrtcType;

typedef struct {
    u8  reg[CRTC_NUM_REGS];
    u8  selected;          /* address register */
    CrtcType type;

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
    u16 ma_next_row;       /* row address captured by the R1 comparator */

    bool hsync;
    bool vsync;
    bool h_display;
    bool v_display;
    bool display_enable;
    bool mode_latch;       /* one-tick GA mode-latch event */
    bool mode_latched_this_hsync;
} CRTC;

void crtc_init(CRTC *c);
void crtc_set_type(CRTC *c, CrtcType type);
void crtc_select(CRTC *c, u8 reg);
void crtc_write(CRTC *c, u8 val);
u8   crtc_read(CRTC *c);
u8   crtc_read_status(CRTC *c);     /* &BE00 — type-dependent status/read port */
void crtc_tick(CRTC *c);       /* one character clock (1 MHz) */

/* Derived helpers */
static inline u16 crtc_screen_addr(CRTC *c) { return c->ma; }
static inline bool crtc_hsync(CRTC *c)      { return c->hsync; }
static inline bool crtc_vsync(CRTC *c)      { return c->vsync; }
static inline bool crtc_de(CRTC *c)         { return c->display_enable; }
static inline bool crtc_mode_latch(CRTC *c) { return c->mode_latch; }

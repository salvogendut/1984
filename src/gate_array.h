#pragma once
#include "types.h"

/*
 * Amstrad Gate Array (40010 / 40007 / 40008)
 * Controls:
 *   - ink/border colours (16 inks + border, hardware palette)
 *   - screen mode (0=160x200/16c, 1=320x200/4c, 2=640x200/2c)
 *   - ROM enable (lower OS, upper BASIC)
 *   - interrupt timer / raster interrupt counter
 *   - RAM banking (6128 only, via additional latch)
 */

#define GA_NUM_INKS 17   /* 16 inks + 1 border */

typedef struct {
    u8  ink[GA_NUM_INKS];    /* hardware colour index (0-31) */
    /* Resolved RGB888 cache of ink[]; kept in sync by ga_init / ga_write so
     * the per-pixel render path can do a single u32 load instead of two
     * dependent array lookups (ink[pen] then ga_hw_palette[idx]). */
    u32 resolved_ink[GA_NUM_INKS];
    u8  selected_pen;        /* current pen register (bit 4 = border) */
    u8  screen_mode;         /* 0/1/2 — active mode (latched on HSYNC) */
    u8  requested_mode;      /* mode written via ga_write; copied to screen_mode on next HSYNC */
    bool lower_rom;          /* OS ROM mapped at 0x0000 */
    bool upper_rom;          /* BASIC ROM mapped at 0xC000 */
    u8  interrupt_counter;   /* counts HSYNCs, fires IRQ at 52 */
    u8  vsync_delay;         /* VSYNC→IRQ-resync countdown (2 HSYNCs); 0 = idle */
    bool interrupt_pending;
} GateArray;

/* Hardware palette: 32 entries, RGB888 */
extern const u32 ga_hw_palette[32];

void ga_init(GateArray *ga);
void ga_write(GateArray *ga, u8 val);   /* port 0x7F (A15=0) */
u8   ga_mode(GateArray *ga);
void ga_hsync(GateArray *ga);           /* called by CRTC on each HSYNC */
void ga_vsync_start(GateArray *ga);     /* called on CRTC VSYNC rising edge */
void ga_irq_ack(GateArray *ga);         /* called by Z80 on IRQ acknowledge */

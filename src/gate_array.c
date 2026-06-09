#include "gate_array.h"
#include <string.h>

/* Standard CPC hardware palette — 32 colours, RGB888.
 * Values from caprice32 colours_rgb[][3] × 255. */
const u32 ga_hw_palette[32] = {
    0x808080, 0x808080, 0x00FF80, 0xFFFF80,
    0x000080, 0xFF0080, 0x008080, 0xFF8080,
    0xFF0080, 0xFFFF80, 0xFFFF00, 0xFFFFFF,
    0xFF0000, 0xFF00FF, 0xFF8000, 0xFF80FF,
    0x000080, 0x00FF80, 0x00FF00, 0x00FFFF,
    0x000000, 0x0000FF, 0x008000, 0x0080FF,
    0x800080, 0x80FF80, 0x80FF00, 0x80FFFF,
    0x800000, 0x8000FF, 0x808000, 0x8080FF,
};

void ga_init(GateArray *ga) {
    memset(ga, 0, sizeof(*ga));
    ga->lower_rom = true;
    ga->upper_rom = true;
    ga->screen_mode = 1;
    ga->requested_mode = 1;
    /* Power-on default inks */
    for (int i = 0; i < GA_NUM_INKS; i++)
        ga->ink[i] = 0;
    ga->ink[0]  = 0x1A;   /* typical border */
    ga->ink[1]  = 0x04;
    for (int i = 0; i < GA_NUM_INKS; i++)
        ga->resolved_ink[i] = ga_hw_palette[ga->ink[i] & 0x1F];
}

void ga_write(GateArray *ga, u8 val) {
    u8 func = (val >> 6) & 0x03;
    switch (func) {
        case 0x00:   /* Select pen */
            ga->selected_pen = val & 0x1F;
            break;
        case 0x01:   /* Set ink colour */
            if (ga->selected_pen < GA_NUM_INKS) {
                ga->ink[ga->selected_pen] = val & 0x1F;
                ga->resolved_ink[ga->selected_pen] = ga_hw_palette[val & 0x1F];
            }
            break;
        case 0x02:   /* Screen mode + ROM control */
            /* Mode change is latched: real hardware only adopts the new mode
             * at the next HSYNC, so mid-line writes don't tear the current
             * character. Demos with raster mode-splits (and the firmware's
             * mode-set sequence) rely on this to avoid flicker. */
            ga->requested_mode = val & 0x03;
            ga->lower_rom   = !(val & 0x04);
            ga->upper_rom   = !(val & 0x08);
            if (val & 0x10) {
                ga->interrupt_counter = 0;
                ga->interrupt_pending = false;
            }
            break;
        case 0x03:   /* RAM configuration (6128) — handled by CPC top level */
            break;
    }
}

u8 ga_mode(GateArray *ga) {
    return ga->screen_mode;
}

void ga_hsync(GateArray *ga) {
    /* Latch the requested mode at start of new scan line */
    ga->screen_mode = ga->requested_mode;
    ga->interrupt_counter++;
    if (ga->interrupt_counter >= 52) {
        ga->interrupt_counter = 0;
        ga->interrupt_pending = true;
    }
    /* VSYNC IRQ resync: 2 HSYNCs after VSYNC begins, if counter >= 32 raise a
     * compensating IRQ, then reset counter. Locks the 300 Hz raster IRQ to
     * VSYNC so the firmware's frame-flyback ticker (INK/BORDER flashing,
     * SOUND queue, etc.) advances at a steady cadence. */
    if (ga->vsync_delay) {
        if (--ga->vsync_delay == 0) {
            if (ga->interrupt_counter >= 32)
                ga->interrupt_pending = true;
            ga->interrupt_counter = 0;
        }
    }
}

void ga_vsync_start(GateArray *ga) {
    ga->vsync_delay = 2;
}

void ga_irq_ack(GateArray *ga) {
    /* On IRQ acknowledge the GA clears bit 5 of the line counter, so a value
     * of ≥32 at ack time drops below the next-IRQ threshold and the firmware
     * gets one IRQ per request instead of two. */
    ga->interrupt_counter &= 0x1F;
}

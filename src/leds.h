#pragma once
#include <SDL3/SDL.h>
#include <stdbool.h>

/* Drive-activity LED bar rendered below the CPC screen.
 *
 * Categories (color coded):
 *   FDC drives (A, B)      - dark red / bright red
 *   IDE (Symbiface/Cyboard) - dark green / bright green
 *   USB/SD (Albireo, M4)   - dark blue / bright blue
 *
 * Activity is signalled by leds_ping_*() from the device emulation; the LED
 * glows bright for LED_GLOW_MS milliseconds after each ping, then fades to
 * its dark "idle" colour.
 */

#define LED_BAR_H     22
#define LED_GLOW_MS   120

typedef enum {
    LED_FDC_A = 0,
    LED_FDC_B,
    LED_IDE,
    LED_USB,
    LED_SD,
    LED_COUNT
} LedId;

/* Configure which LEDs to display in the bar. Call after reading config. */
void leds_set_enabled(LedId id, bool enabled);

/* Signal one frame of activity for the given LED. */
void leds_ping(LedId id);

/* Render the LED bar across (x,y,w,h). The caller has already cleared the
 * renderer and drawn the CPC screen above this rect. */
void leds_render(SDL_Renderer *r, int x, int y, int w, int h);

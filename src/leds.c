#include "leds.h"
#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t dr, dg, db;   /* idle (dark)  colour */
    uint8_t br, bg, bb;   /* active (bright) colour */
} LedPalette;

static const LedPalette palette[LED_COUNT] = {
    [LED_FDC_A]  = { 70, 18, 18,  255,  70,  70 },
    [LED_FDC_B]  = { 70, 18, 18,  255,  70,  70 },
    [LED_IDE]    = { 18, 70, 18,   80, 255,  80 },
    [LED_USB]    = { 25, 50,110,   90, 160, 255 },
    [LED_SD]     = { 25, 50,110,   90, 160, 255 },
    [LED_NET]    = { 80, 70, 18,  255, 224,  32 },   /* Net4CPC — yellow */
    [LED_USIFAC] = { 0 },   /* Split LED renders both halves; entries unused. */
};

/* Idle/active colours for the split-LED halves (USIfAC RS232). The left
 * half is red and signals RX activity; the right half is green and signals
 * TX. Colours chosen to read at a glance against the dark LED bar. */
static const LedPalette palette_usifac_rx = { 70, 18, 18,  255,  70,  70 };
static const LedPalette palette_usifac_tx = { 18, 70, 18,   80, 255,  80 };

static bool   g_enabled  [LED_COUNT];
static Uint64 g_last_ms  [LED_COUNT];   /* Generic, also used for LED_USIFAC RX half */
static Uint64 g_last_ms_b[LED_COUNT];   /* Only used for split LEDs (TX half) */

void leds_set_enabled(LedId id, bool enabled) {
    if ((unsigned)id < LED_COUNT) g_enabled[id] = enabled;
}

void leds_ping(LedId id) {
    if ((unsigned)id < LED_COUNT) g_last_ms[id] = SDL_GetTicks();
}

void leds_ping_split(LedId id, bool tx) {
    if ((unsigned)id < LED_COUNT)
        (tx ? g_last_ms_b : g_last_ms)[id] = SDL_GetTicks();
}

void leds_render(SDL_Renderer *r, int x, int y, int w, int h) {
    /* Bar background */
    SDL_SetRenderDrawColor(r, 18, 18, 18, 255);
    SDL_FRect bg = { (float)x, (float)y, (float)w, (float)h };
    SDL_RenderFillRect(r, &bg);

    /* Top hairline separator */
    SDL_SetRenderDrawColor(r, 50, 50, 50, 255);
    SDL_FRect line = { (float)x, (float)y, (float)w, 1.0f };
    SDL_RenderFillRect(r, &line);

    /* Count enabled LEDs to centre them in the bar */
    int n = 0;
    for (int i = 0; i < LED_COUNT; i++) if (g_enabled[i]) n++;
    if (n == 0) return;

    const int led_w   = 24;
    const int led_h   = 10;
    const int pad     = 8;
    const int total_w = n * led_w + (n - 1) * pad;
    int       cx      = x + (w - total_w) / 2;
    const int cy      = y + (h - led_h) / 2;

    Uint64 now = SDL_GetTicks();
    for (int i = 0; i < LED_COUNT; i++) {
        if (!g_enabled[i]) continue;

        SDL_FRect led = { (float)cx, (float)cy, (float)led_w, (float)led_h };

        if (i == LED_USIFAC) {
            /* Split LED: left half red (RX), right half green (TX). Same
             * outer footprint as a normal LED so the bar layout is
             * unchanged. */
            const int half_w = led_w / 2;
            for (int side = 0; side < 2; side++) {
                const LedPalette *p = side == 0 ? &palette_usifac_rx
                                                : &palette_usifac_tx;
                Uint64 ts = side == 0 ? g_last_ms[i] : g_last_ms_b[i];
                bool active = ts != 0 && (now - ts) < LED_GLOW_MS;
                uint8_t R = active ? p->br : p->dr;
                uint8_t G = active ? p->bg : p->dg;
                uint8_t B = active ? p->bb : p->db;
                SDL_FRect half = { (float)(cx + side * half_w), (float)cy,
                                   (float)half_w, (float)led_h };
                SDL_SetRenderDrawColor(r, R, G, B, 255);
                SDL_RenderFillRect(r, &half);
            }
            /* Single outline around the whole footprint */
            SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
            SDL_RenderRect(r, &led);
        } else {
            const LedPalette *p = &palette[i];
            Uint64 dt = now - g_last_ms[i];
            bool active = g_last_ms[i] != 0 && dt < LED_GLOW_MS;
            uint8_t R = active ? p->br : p->dr;
            uint8_t G = active ? p->bg : p->dg;
            uint8_t B = active ? p->bb : p->db;

            SDL_SetRenderDrawColor(r, R, G, B, 255);
            SDL_RenderFillRect(r, &led);

            /* 1-px dark outline to give the LED a defined edge against the bar */
            SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
            SDL_RenderRect(r, &led);
        }

        cx += led_w + pad;
    }
}

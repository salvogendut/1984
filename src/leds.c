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
    [LED_M4]     = { 0 },   /* Triple LED renders three segments; entries unused. */
};

/* Idle/active colours for the split-LED halves (USIfAC RS232). The left
 * half is red and signals RX activity; the right half is green and signals
 * TX. Colours chosen to read at a glance against the dark LED bar. */
static const LedPalette palette_usifac_rx = { 70, 18, 18,  255,  70,  70 };
static const LedPalette palette_usifac_tx = { 18, 70, 18,   80, 255,  80 };

/* M4 LED: three segments left-to-right.
 *   power : blue,  steady-on while M4 enabled (no idle state used)
 *   disk  : red,   ping on SD / file API command
 *   net   : white, ping on M4 networking command */
static const LedPalette palette_m4_power = { 25, 50,110,   90, 160, 255 };
static const LedPalette palette_m4_disk  = { 70, 18, 18,  255,  70,  70 };
static const LedPalette palette_m4_net   = { 70, 70, 70,  240, 240, 240 };

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

void leds_ping_m4_disk(void) { g_last_ms  [LED_M4] = SDL_GetTicks(); }
void leds_ping_m4_net (void) { g_last_ms_b[LED_M4] = SDL_GetTicks(); }

void leds_render(SDL_Renderer *r, int x, int y, int w, int h) {
    /* Bar background */
    SDL_SetRenderDrawColor(r, 18, 18, 18, 255);
    SDL_FRect bg = { (float)x, (float)y, (float)w, (float)h };
    SDL_RenderFillRect(r, &bg);

    /* Top hairline separator */
    SDL_SetRenderDrawColor(r, 50, 50, 50, 255);
    SDL_FRect line = { (float)x, (float)y, (float)w, 1.0f };
    SDL_RenderFillRect(r, &line);

    const int led_w    = 24;
    const int led_w_m4 = led_w * 3 / 2;     /* M4 is 1.5× wide */
    const int led_h    = 10;
    const int pad      = 8;

    /* Sum widths + padding for centring. LED_M4 contributes the wider
     * footprint; everything else uses led_w. */
    int n = 0, total_w = 0;
    for (int i = 0; i < LED_COUNT; i++) {
        if (!g_enabled[i]) continue;
        total_w += (i == LED_M4) ? led_w_m4 : led_w;
        n++;
    }
    if (n == 0) return;
    total_w += (n - 1) * pad;

    int       cx = x + (w - total_w) / 2;
    const int cy = y + (h - led_h) / 2;

    Uint64 now = SDL_GetTicks();
    for (int i = 0; i < LED_COUNT; i++) {
        if (!g_enabled[i]) continue;

        const int this_w = (i == LED_M4) ? led_w_m4 : led_w;
        SDL_FRect led = { (float)cx, (float)cy, (float)this_w, (float)led_h };

        if (i == LED_M4) {
            /* Triple-segment LED: blue power (steady), red disk, white net.
             * Three equal slices, single outline around the whole footprint. */
            const int seg_w = led_w_m4 / 3;
            const LedPalette *segs[3] = {
                &palette_m4_power, &palette_m4_disk, &palette_m4_net
            };
            for (int s = 0; s < 3; s++) {
                const LedPalette *p = segs[s];
                bool active;
                if (s == 0) {
                    active = true;                          /* power: always on */
                } else {
                    Uint64 ts = (s == 1) ? g_last_ms[i] : g_last_ms_b[i];
                    active = ts != 0 && (now - ts) < LED_GLOW_MS;
                }
                uint8_t R = active ? p->br : p->dr;
                uint8_t G = active ? p->bg : p->dg;
                uint8_t B = active ? p->bb : p->db;
                SDL_FRect seg = { (float)(cx + s * seg_w), (float)cy,
                                  (float)seg_w, (float)led_h };
                SDL_SetRenderDrawColor(r, R, G, B, 255);
                SDL_RenderFillRect(r, &seg);
            }
            SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
            SDL_RenderRect(r, &led);
        } else if (i == LED_USIFAC) {
            /* Split LED: left half red (RX), right half green (TX). Same
             * outer footprint as a normal LED so the bar layout is
             * unchanged. */
            const int half_w = this_w / 2;
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

        cx += this_w + pad;
    }
}

/* Minimal WASM proof-of-concept main for the 1984 core.
 *
 * Exposes a tiny C API to the browser glue:
 *   - poc_init():  boot a plain CPC 6128 from the embedded ROMs
 *   - poc_step():  run one emulated frame (monitor completes one frame)
 *   - poc_pixels(): pointer to the 768x272 framebuffer (u32 ARGB-ish)
 *   - poc_key():   SDL_Scancode key down/up
 *
 * No SDL runtime is used — the browser reads the framebuffer directly and
 * renders it to a canvas; the SDL3 headers are only pulled in for the type
 * definitions (Display holds SDL pointers, kbd_sdl_key takes SDL_Scancode).
 */
#include <emscripten.h>
#include "cpc.h"
#include "kbd.h"

static CPC g_cpc;
static int g_inited = 0;

EMSCRIPTEN_KEEPALIVE int poc_init(void) {
    if (g_inited) return 0;
    if (cpc_init(&g_cpc, MODEL_6128, "roms/OS_6128.ROM",
                 "roms/BASIC_1.1.ROM", NULL, 1) != 0)
        return -1;
    g_inited = 1;
    return 0;
}

EMSCRIPTEN_KEEPALIVE void poc_step(void) {
    cpc_frame(&g_cpc);
}

EMSCRIPTEN_KEEPALIVE unsigned int *poc_pixels(void) {
    return g_cpc.display.pixels;
}

EMSCRIPTEN_KEEPALIVE void poc_key(int scancode, int pressed) {
    kbd_sdl_key(&g_cpc.kbd, (SDL_Scancode)scancode, pressed != 0);
}

EMSCRIPTEN_KEEPALIVE int poc_width(void)  { return CPC_SCREEN_W; }
EMSCRIPTEN_KEEPALIVE int poc_height(void) { return CPC_SCREEN_H; }

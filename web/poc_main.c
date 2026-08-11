/* Minimal WASM proof-of-concept main for the 1984 core.
 *
 * Exposes a tiny C API to the browser glue:
 *   - poc_init():     boot a plain CPC 6128 from the embedded ROMs
 *   - poc_step():     run one emulated frame (monitor completes one frame)
 *   - poc_pixels():   pointer to the 768x272 framebuffer (u32 0x00RRGGBB)
 *   - poc_key():      SDL_Scancode key down/up
 *   - poc_load_disk(): mount a .dsk into drive A from the virtual FS
 *   - poc_autorun():  queue RUN"file" after a frame-counted boot delay
 *   - poc_mouse_*():  browser pointer input through the AMX adapter
 *   - poc_audio_*():  ring-buffer access for the PSG audio (interleaved s16)
 *
 * No SDL runtime is used — the browser reads the framebuffer and the audio
 * ring buffer directly from wasm memory; the SDL3 headers are only pulled in
 * for the type definitions (Display holds SDL pointers, kbd_sdl_key takes
 * SDL_Scancode).
 */
#include <emscripten.h>
#include <stdio.h>
#include <string.h>
#include "cpc.h"
#include "kbd.h"
#include "disk.h"
#include "mem.h"
#include "paste.h"

static CPC g_cpc;
static Paste g_paste;
static int g_autorun_frames;
static char g_autorun_command[256];

static void poc_cancel_paste(void) {
    paste_free(&g_paste);
    paste_init(&g_paste);
    g_autorun_frames = 0;
    g_autorun_command[0] = '\0';
}

/* ---- audio ring buffer (interleaved stereo s16, 2 seconds @ 44.1 kHz) ---- */
#define AUDIO_RING_SAMPLES (44100 * 4)
static s16 g_audio_ring[AUDIO_RING_SAMPLES];
static int g_audio_w = 0;   /* write index (producer = emulator) */
static int g_audio_r = 0;   /* read index  (consumer = browser)  */

static void poc_audio_sink(void *userdata, const s16 *samples, int frames,
                           int sample_rate) {
    (void)userdata; (void)sample_rate;
    int n = frames * 2;
    for (int i = 0; i < n; i++) {
        int w = (g_audio_w + 1) % AUDIO_RING_SAMPLES;
        if (w == g_audio_r) break;            /* full — drop this frame */
        g_audio_ring[g_audio_w] = samples[i];
        g_audio_w = w;
    }
}

EMSCRIPTEN_KEEPALIVE void poc_audio_reset(void) { g_audio_w = 0; g_audio_r = 0; }
EMSCRIPTEN_KEEPALIVE int  poc_audio_avail(void) {
    return (g_audio_w - g_audio_r + AUDIO_RING_SAMPLES) % AUDIO_RING_SAMPLES;
}
EMSCRIPTEN_KEEPALIVE int  poc_audio_read_pos(void) { return g_audio_r; }
EMSCRIPTEN_KEEPALIVE short *poc_audio_buffer(void) { return g_audio_ring; }
EMSCRIPTEN_KEEPALIVE void poc_audio_advance(int n) {
    g_audio_r = (g_audio_r + n) % AUDIO_RING_SAMPLES;
}

/* ---- emulator lifecycle ----
 * model 0 = CPC 6128 (OS/BASIC/AMSDOS), 1 = CPC 6128 Plus (cartridge;
 * defaults to the bundled system cartridge when cartridge is NULL).
 * May be called repeatedly to switch machines. */

EMSCRIPTEN_KEEPALIVE int poc_init_model(int model, const char *cartridge) {
    int rc;
    poc_cancel_paste();
    if (model == 1) {
        rc = cpc_init(&g_cpc, MODEL_6128_PLUS, NULL, NULL,
                      cartridge ? cartridge : "roms/system.cpr", 1);
    } else {
        rc = cpc_init(&g_cpc, MODEL_6128, "roms/OS_6128.ROM",
                      "roms/BASIC_1.1.ROM", NULL, 1);
        if (rc == 0)
            mem_load_amsdos(&g_cpc.mem, "roms/AMSDOS.ROM");
    }
    if (rc != 0)
        return -1;
    cpc_set_audio_sink(&g_cpc, poc_audio_sink, NULL);
    return 0;
}

EMSCRIPTEN_KEEPALIVE int poc_init(void) { return poc_init_model(0, NULL); }

EMSCRIPTEN_KEEPALIVE int poc_load_cartridge(const char *path) {
    return poc_init_model(1, path);
}

/* Warm reset of the current machine (keeps loaded ROMs/cartridge). */
EMSCRIPTEN_KEEPALIVE void poc_reset(void) {
    poc_cancel_paste();
    cpc_reset(&g_cpc);
}

EMSCRIPTEN_KEEPALIVE int poc_step(void) {
    if (g_autorun_frames > 0 && --g_autorun_frames == 0)
        paste_text(&g_paste, g_autorun_command);
    paste_tick(&g_paste, &g_cpc.kbd);
    return cpc_frame(&g_cpc);
}

EMSCRIPTEN_KEEPALIVE unsigned int *poc_pixels(void) {
    return g_cpc.display.pixels;
}

EMSCRIPTEN_KEEPALIVE void poc_key(int scancode, int pressed) {
    kbd_sdl_key(&g_cpc.kbd, (SDL_Scancode)scancode, pressed != 0);
}

EMSCRIPTEN_KEEPALIVE int poc_load_disk(const char *path) {
    disk_eject(&g_cpc.drive[0]);
    if (disk_load(&g_cpc.drive[0], path) != 0)
        return -1;
    return 0;
}

EMSCRIPTEN_KEEPALIVE void poc_eject_disk(void) {
    disk_eject(&g_cpc.drive[0]);
}

/* Queue RUN"filename" after a frame-counted boot delay. This mirrors the
 * native --autostart path instead of synthesizing browser key events. */
EMSCRIPTEN_KEEPALIVE int poc_autorun(const char *filename, int delay_frames) {
    if (!filename || !filename[0])
        return -1;
    size_t len = strlen(filename);
    if (len > 240)
        return -1;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)filename[i];
        if (c < 0x20 || c == 0x7f || c == '"')
            return -1;
    }

    poc_cancel_paste();
    int written = snprintf(g_autorun_command, sizeof(g_autorun_command),
                           "run\"%s", filename);
    if (written < 0 || written >= (int)sizeof(g_autorun_command)) {
        g_autorun_command[0] = '\0';
        return -1;
    }
    g_autorun_frames = delay_frames > 0 ? delay_frames : 42;
    return 0;
}

/* CPC joystick 1 = keyboard matrix row 9, bits 0-5 (Up Down Left Right F1 F2). */
#define POC_JOY_ROW 9
EMSCRIPTEN_KEEPALIVE void poc_joy(int col, int pressed) {
    if (col < 0 || col > 5) return;
    if (pressed) kbd_key_down(&g_cpc.kbd, POC_JOY_ROW, col);
    else         kbd_key_up  (&g_cpc.kbd, POC_JOY_ROW, col);
}

/* Diagnostic readback used by the browser to distinguish Gamepad API mapping
 * failures from CPC-side input failures. Row 9 is active low. */
EMSCRIPTEN_KEEPALIVE int poc_joy_matrix(void) {
    return g_cpc.kbd.matrix[POC_JOY_ROW];
}

/* Browser pointer input uses the AMX adapter on joystick port 1. The AMX
 * mouse and a physical joystick share matrix row 9, so the web UI presents
 * them as mutually exclusive input channels. */
EMSCRIPTEN_KEEPALIVE void poc_set_mouse(int enabled) {
    amx_reset(&g_cpc.amx, &g_cpc.kbd);
    g_cpc.amx_mouse = enabled != 0;
}

EMSCRIPTEN_KEEPALIVE void poc_mouse_move(int dx, int dy) {
    if (g_cpc.amx_mouse)
        amx_move(&g_cpc.amx, dx, dy);
}

EMSCRIPTEN_KEEPALIVE void poc_mouse_button(int button, int pressed) {
    if (g_cpc.amx_mouse)
        amx_button(&g_cpc.amx, &g_cpc.kbd, button, pressed != 0);
}

/* Disk activity: the FDC motor spins while a floppy is being accessed. */
EMSCRIPTEN_KEEPALIVE int poc_disk_motor(void) { return g_cpc.fdc.motor ? 1 : 0; }

EMSCRIPTEN_KEEPALIVE int poc_width(void)  { return CPC_SCREEN_W; }
EMSCRIPTEN_KEEPALIVE int poc_height(void) { return CPC_SCREEN_H; }

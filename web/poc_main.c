/* Minimal WASM proof-of-concept main for the 1984 core.
 *
 * Exposes a tiny C API to the browser glue:
 *   - poc_init():     boot a plain CPC 6128 from the embedded ROMs
 *   - poc_step():     run one emulated frame (monitor completes one frame)
 *   - poc_pixels():   pointer to the 768x272 framebuffer (u32 0x00RRGGBB)
 *   - poc_key():      SDL_Scancode key down/up
 *   - poc_load_disk(): mount a .dsk into drive A from the virtual FS
 *   - poc_audio_*():  ring-buffer access for the PSG audio (interleaved s16)
 *
 * No SDL runtime is used — the browser reads the framebuffer and the audio
 * ring buffer directly from wasm memory; the SDL3 headers are only pulled in
 * for the type definitions (Display holds SDL pointers, kbd_sdl_key takes
 * SDL_Scancode).
 */
#include <emscripten.h>
#include "cpc.h"
#include "kbd.h"
#include "disk.h"
#include "mem.h"

static CPC g_cpc;
static int g_inited = 0;

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

/* ---- emulator lifecycle ---- */

EMSCRIPTEN_KEEPALIVE int poc_init(void) {
    if (g_inited) return 0;
    g_debug_enabled = 1;   /* allow ONE_K_TRACE_* diagnostics in the wasm */
    if (cpc_init(&g_cpc, MODEL_6128, "roms/OS_6128.ROM",
                 "roms/BASIC_1.1.ROM", NULL, 1) != 0)
        return -1;
    /* AMSDOS is required to use floppy disks (|CPM, RUN "..." from a .dsk). */
    /* TEMP: skip AMSDOS to isolate the boot freeze
    if (mem_load_amsdos(&g_cpc.mem, "roms/AMSDOS.ROM") != 0)
        return -2; */
    cpc_set_audio_sink(&g_cpc, poc_audio_sink, NULL);
    g_inited = 1;
    return 0;
}

EMSCRIPTEN_KEEPALIVE int poc_step(void) {
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

EMSCRIPTEN_KEEPALIVE int poc_width(void)  { return CPC_SCREEN_W; }
EMSCRIPTEN_KEEPALIVE int poc_height(void) { return CPC_SCREEN_H; }

/* ---- temporary diagnostics ---- */
extern long long g_total_t;
extern int g_debug_enabled;
extern int g_poc_trace_pc;
EMSCRIPTEN_KEEPALIVE void poc_set_trace_pc(int on) { g_poc_trace_pc = on; }
EMSCRIPTEN_KEEPALIVE int poc_mem(unsigned addr)      { return mem_read(&g_cpc.mem, (u16)addr); }
EMSCRIPTEN_KEEPALIVE int poc_upper_sel(void)         { return g_cpc.mem.upper_rom_select; }
EMSCRIPTEN_KEEPALIVE int poc_upper_en(void)          { return g_cpc.mem.upper_rom_enabled; }
EMSCRIPTEN_KEEPALIVE int poc_lower_en(void)          { return g_cpc.mem.lower_rom_enabled; }
EMSCRIPTEN_KEEPALIVE int poc_rom_basic0(void)        { return g_cpc.mem.rom_basic[0]; }
EMSCRIPTEN_KEEPALIVE int poc_rom_os0(void)           { return g_cpc.mem.rom_os[0]; }
extern int g_poc_crtc_ticks;
extern int g_poc_monitor_advances;
extern int g_poc_monitor_peaks;
extern int g_poc_ga_hsync_calls;
extern int g_poc_ga_irq_fires;
EMSCRIPTEN_KEEPALIVE int poc_ga_hsync_calls(void)   { return g_poc_ga_hsync_calls; }
EMSCRIPTEN_KEEPALIVE int poc_ga_irq_fires(void)     { return g_poc_ga_irq_fires; }
EMSCRIPTEN_KEEPALIVE int poc_crtc_ticks(void)      { return g_poc_crtc_ticks; }
EMSCRIPTEN_KEEPALIVE int poc_mon_advances(void)    { return g_poc_monitor_advances; }
EMSCRIPTEN_KEEPALIVE int poc_mon_peaks(void)       { return g_poc_monitor_peaks; }
EMSCRIPTEN_KEEPALIVE unsigned int poc_pc(void)         { return g_cpc.cpu.pc; }
EMSCRIPTEN_KEEPALIVE int poc_irq_counter(void)          { return g_cpc.ga.interrupt_counter; }
EMSCRIPTEN_KEEPALIVE int poc_irq_pending(void)          { return g_cpc.ga.interrupt_pending; }
EMSCRIPTEN_KEEPALIVE int poc_frame_count(void)          { return cpc_frame_count; }
EMSCRIPTEN_KEEPALIVE int poc_monitor_completed(void)    { return g_cpc.monitor_frame_completed; }
EMSCRIPTEN_KEEPALIVE long long poc_total(void)          { return g_total_t; }
EMSCRIPTEN_KEEPALIVE int poc_paused(void)               { return g_cpc.paused; }
EMSCRIPTEN_KEEPALIVE int poc_hcc(void)                  { return g_cpc.crtc.hcc; }
EMSCRIPTEN_KEEPALIVE int poc_vcc(void)                  { return g_cpc.crtc.vcc; }
EMSCRIPTEN_KEEPALIVE int poc_vlc(void)                  { return g_cpc.crtc.vlc; }
EMSCRIPTEN_KEEPALIVE int poc_vline(void)                { return g_cpc.monitor_vline; }
EMSCRIPTEN_KEEPALIVE int poc_raster_y(void)             { return g_cpc.raster_y; }
EMSCRIPTEN_KEEPALIVE int poc_hsync_reg(void)            { return g_cpc.monitor_hsync; }
EMSCRIPTEN_KEEPALIVE int poc_hpos(void)                 { return g_cpc.monitor_hpos; }
EMSCRIPTEN_KEEPALIVE int poc_in_hsync(void)             { return g_cpc.monitor_in_hsync; }
EMSCRIPTEN_KEEPALIVE int poc_crtc_reg(int i)            { return g_cpc.crtc.reg[i & 31]; }
EMSCRIPTEN_KEEPALIVE int poc_crtc_hsync(void)           { return g_cpc.crtc.hsync; }
EMSCRIPTEN_KEEPALIVE int poc_crtc_hsc(void)             { return g_cpc.crtc.hsc; }
EMSCRIPTEN_KEEPALIVE int poc_z_pending(void)            { return g_cpc.cpu.pending_irq; }
EMSCRIPTEN_KEEPALIVE int poc_z_iff1(void)               { return g_cpc.cpu.iff1; }
EMSCRIPTEN_KEEPALIVE int poc_z_accepted(void)           { return g_cpc.cpu.int_accepted; }

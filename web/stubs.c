/* Stubs for host-dependent modules excluded from the WASM POC build.
 *
 * The POC only boots a plain CPC to BASIC with video/keyboard, so the real
 * host device code (real tape, networking, PTY, printer/cairo, serial) is
 * replaced by no-op or minimal implementations that satisfy the linker.
 * display_finalize_frame is implemented for real: it fills framebuffer pixels
 * the monitor beam did not touch with the border colour.
 */
#include "cpc.h"
#include "real_tape.h"
#include "net4cpc.h"
#include "kbd_pty.h"
#include "printer.h"
#include "usifac.h"
#include "perryfi.h"
#include "leds.h"

/* ---- LEDs ---- */
void leds_set_enabled(LedId id, bool enabled) { (void)id; (void)enabled; }
void leds_ping(LedId id) { (void)id; }
void leds_ping_split(LedId id, bool tx) { (void)id; (void)tx; }
void leds_ping_m4_disk(void) {}
void leds_ping_m4_net(void) {}
void leds_set_mouse_position(int x, int y, bool inside) { (void)x; (void)y; (void)inside; }

/* ---- SDL audio stream (cpc.c's audio output path; POC has no audio yet) ---- */
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid,
                                           const SDL_AudioSpec *spec,
                                           SDL_AudioStreamCallback callback,
                                           void *userdata) {
    (void)devid; (void)spec; (void)callback; (void)userdata;
    return NULL;
}
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream) { (void)stream; return false; }
bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *buf, int len) {
    (void)stream; (void)buf; (void)len; return false;
}
int SDL_GetAudioStreamQueued(SDL_AudioStream *stream) { (void)stream; return 0; }

/* ---- display (SDL-free subset) ---- */
int display_init(Display *d, const char *title, int scale) {
    (void)d; (void)title; (void)scale; return 0;
}
void display_destroy(Display *d) { (void)d; }
void display_upload(Display *d) { (void)d; }
void display_flip(Display *d) { (void)d; }
void display_put_pixel(Display *d, u32 rgb) { (void)d; (void)rgb; }
void display_next_line(Display *d) { (void)d; }
void display_vsync(Display *d) { (void)d; }
void display_finalize_frame(Display *d, u32 blank) {
    int n = CPC_SCREEN_W * CPC_SCREEN_H;
    for (int i = 0; i < n; i++) {
        if (!d->touched[i])
            d->pixels[i] = blank;
        d->touched[i] = 0;
    }
}
void display_save_ppm(Display *d, const char *path) { (void)d; (void)path; }
u32 display_hash(Display *d) { (void)d; return 0; }
void display_copy_visible(Display *d, u32 *dst) { (void)d; (void)dst; }
bool display_changed_rect(Display *d, u32 *prev, int *x, int *y, int *w, int *h) {
    (void)d; (void)prev; (void)x; (void)y; (void)w; (void)h; return false;
}
bool display_save_crop_ppm(Display *d, const char *path, int x, int y, int w, int h, int scale) {
    (void)d; (void)path; (void)x; (void)y; (void)w; (void)h; (void)scale; return false;
}
void display_set_smoothing(Display *d, bool smooth) { (void)d; (void)smooth; }
void display_set_crt(Display *d, bool enabled, int scanlines, int brightness,
                     int contrast, int red, int green, int blue) {
    (void)d; (void)enabled; (void)scanlines; (void)brightness;
    (void)contrast; (void)red; (void)green; (void)blue;
}
void display_apply_greyscale(Display *d) { (void)d; }
void display_draw_paused_label(Display *d) { (void)d; }
int  display_window_id(Display *d) { (void)d; return 0; }

/* ---- video/audio capture (not used in the POC) ---- */
bool videocap_start(const char *path, int w, int fps, bool ffmpeg) {
    (void)path; (void)w; (void)fps; (void)ffmpeg; return false;
}
void videocap_stop(void) {}
bool videocap_active(void) { return false; }
int  videocap_frame_count(void) { return 0; }
bool audiocap_start(const char *path) { (void)path; return false; }
void audiocap_stop(void) {}
bool audiocap_active(void) { return false; }
void audiocap_write(const s16 *samples, int frames, int sample_rate) {
    (void)samples; (void)frames; (void)sample_rate;
}

/* ---- real tape ---- */
void real_tape_init(RealTape *rt) { (void)rt; }
void real_tape_shutdown(RealTape *rt) { (void)rt; }
void real_tape_reset(RealTape *rt) { (void)rt; }
void real_tape_pump(RealTape *rt) { (void)rt; }
void real_tape_sample(RealTape *rt, u8 ppi_port_c) { (void)rt; (void)ppi_port_c; }
void real_tape_output_sample(RealTape *rt, u8 tape_level) { (void)rt; (void)tape_level; }
void real_tape_flush_output(RealTape *rt) { (void)rt; }
bool real_tape_input_active(const RealTape *rt) { (void)rt; return false; }
bool real_tape_connected_input_active(const RealTape *rt) { (void)rt; return false; }
bool real_tape_audible_monitor_enabled(const RealTape *rt) { (void)rt; return false; }
bool real_tape_visual_monitor_enabled(const RealTape *rt) { (void)rt; return false; }
u8 real_tape_input_level(const RealTape *rt) { (void)rt; return 0; }
int real_tape_signal_percent(const RealTape *rt) { (void)rt; return 0; }
int real_tape_buffered_ms(const RealTape *rt) { (void)rt; return 0; }
const char *real_tape_error(const RealTape *rt) { (void)rt; return ""; }
s16 real_tape_monitor_sample(const RealTape *rt) { (void)rt; return 0; }
bool real_tape_source_load_wav(RealTape *rt, const char *path) { (void)rt; (void)path; return false; }
void real_tape_source_eject(RealTape *rt) { (void)rt; }
bool real_tape_source_loaded(const RealTape *rt) { (void)rt; return false; }
const char *real_tape_source_path(const RealTape *rt) { (void)rt; return ""; }
int real_tape_source_progress(const RealTape *rt) { (void)rt; return 0; }
u32 real_tape_source_remaining_seconds(const RealTape *rt) { (void)rt; return 0; }
const char *real_tape_mode_name(RealTapeMode mode) { (void)mode; return "off"; }
bool real_tape_mode_parse(const char *text, RealTapeMode *mode) { (void)text; if (mode) *mode = REAL_TAPE_OFF; return true; }
bool real_tape_mode_has_input(RealTapeMode mode) { (void)mode; return false; }
bool real_tape_mode_has_output(RealTapeMode mode) { (void)mode; return false; }
const char *real_tape_output_source_name(RealTapeOutputSource source) { (void)source; return ""; }
const char *real_tape_output_target_name(RealTapeOutputTarget target) { (void)target; return ""; }
bool real_tape_record_start(RealTape *rt, const char *path) { (void)rt; (void)path; return false; }
void real_tape_record_stop(RealTape *rt) { (void)rt; }

/* ---- net4cpc ---- */
void net4cpc_reset(void) {}
u8 net4cpc_in(u8 reg_sel) { (void)reg_sel; return 0xFF; }
void net4cpc_out(u8 reg_sel, u8 val) { (void)reg_sel; (void)val; }
int net4cpc_attach_tap(const char *devname) { (void)devname; return -1; }
void net4cpc_poll(void) {}

/* ---- kbd_pty ---- */
const char *kbd_pty_open(void) { return NULL; }
void kbd_pty_tick(Paste *p) { (void)p; }
bool kbd_pty_is_open(void) { return false; }
void kbd_pty_emit_char(unsigned char c) { (void)c; }
void kbd_pty_emit_buf(const void *buf, int len) { (void)buf; (void)len; }

/* ---- printer ---- */
void printer_init(Printer *p) { (void)p; }
void printer_shutdown(Printer *p) { (void)p; }
void printer_set_pdf_output_dir(Printer *p, const char *dir) { (void)p; (void)dir; }
void printer_set_pdf_enabled(Printer *p, bool enabled) { (void)p; (void)enabled; }
void printer_set_sink(Printer *p, PrintSink sink) { (void)p; (void)sink; }
void printer_set_connected(Printer *p, bool connected) { (void)p; (void)connected; }
void printer_out(Printer *p, u8 val) { (void)p; (void)val; }
void printer_tick(Printer *p) { (void)p; }

/* ---- usifac ---- */
void usifac_init(USIfAC *u, bool enable, const char *backend, int tcp_port,
                 const char *pty_link) {
    (void)u; (void)enable; (void)backend; (void)tcp_port; (void)pty_link;
}
void usifac_shutdown(USIfAC *u) { (void)u; }
void usifac_attach_perryfi(USIfAC *u, struct Perryfi *p) { (void)u; (void)p; }
u8 usifac_read(USIfAC *u, u8 lo) { (void)u; (void)lo; return 0xFF; }
void usifac_write(USIfAC *u, u8 lo, u8 val) { (void)u; (void)lo; (void)val; }
void usifac_poll(USIfAC *u) { (void)u; }

/* ---- perryfi ---- */
void perryfi_init(Perryfi *p, bool enable) { (void)p; (void)enable; }
void perryfi_shutdown(Perryfi *p) { (void)p; }
void perryfi_poll(Perryfi *p) { (void)p; }
bool perryfi_rx_pop(Perryfi *p, u8 *out) { (void)p; (void)out; return false; }
bool perryfi_tx_push(Perryfi *p, u8 b) { (void)p; (void)b; return false; }
bool perryfi_rx_has(const Perryfi *p) { (void)p; return false; }

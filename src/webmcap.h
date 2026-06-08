/* webmcap.h — WebM (VP9) screen capture via ffmpeg subprocess pipe.
 *
 * We do not link any codec library. Raw BGRA frames are written to
 * ffmpeg's stdin; ffmpeg scales (768x272 → 768x576), encodes VP9 at a
 * modest bitrate, and muxes into Matroska/WebM. The user only needs
 * the ffmpeg binary at runtime — detected by ./configure and baked in
 * as FFMPEG_PATH.
 *
 * On builds where ffmpeg was not present at configure time, HAVE_FFMPEG
 * is undefined and the overlay hides this option. The compiled code in
 * webmcap.c still builds (it just refuses to start the process so users
 * who install ffmpeg later can rebuild without source changes).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct WebmCap WebmCap;

/* Spawn ffmpeg piping VP9-encoded WebM to `path`. Frames passed to
 * webmcap_frame() are assumed to be in_w*in_h BGRA32, encoded at 50 fps
 * and scaled to out_w*out_h before encoding. Returns NULL on failure
 * (no ffmpeg at runtime, popen failure, …). */
WebmCap *webmcap_open(const char *path,
                      int in_w, int in_h,
                      int out_w, int out_h);

/* Submit one BGRA frame. Returns true on success; false ends the
 * recording (caller should stop). */
bool webmcap_frame(WebmCap *w, const uint32_t *pixels);

/* Close the pipe and wait for ffmpeg to finish writing the file. */
void webmcap_close(WebmCap *w);

int  webmcap_frame_count(const WebmCap *w);

/* Compile-time gate matching the AC_PATH_PROG result. */
#ifdef HAVE_FFMPEG
#  define WEBMCAP_SUPPORTED 1
#else
#  define WEBMCAP_SUPPORTED 0
#endif

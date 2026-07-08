/* gifcap.h — in-tree GIF89a screen-capture encoder.
 *
 * Self-contained: no libgif, no ffmpeg, no codec dependencies. Writes a
 * GIF89a stream with one local colour table per frame, LZW-compressed,
 * looping forever (NETSCAPE2.0 extension).
 *
 * Designed for the CPC framebuffer: ≤27 simultaneous colours fits
 * trivially under GIF's 256-colour ceiling, so the per-frame palette is
 * built losslessly from the unique RGB values in each frame. If the
 * frame (overlay open, palette-cycling effect, …) ever exceeds 256
 * unique colours we fall back to RGB332 uniform quantisation.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct GifCap GifCap;

/* Open path for writing. in_w/in_h are the framebuffer dimensions
 * passed to gifcap_frame(); out_w/out_h are the dimensions written into
 * the GIF (nearest-neighbour scaled). Pass out_w==in_w and out_h==in_h
 * for no scaling. frame_delay_cs is the inter-frame delay in
 * centiseconds (1/100 s); use 4 for 25 fps, 2 for 50 fps. Returns NULL
 * on failure. */
GifCap *gifcap_open(const char *path,
                    int in_w, int in_h, int out_w, int out_h,
                    int frame_delay_cs);

/* Append one frame. pixels is in_w*in_h u32 values in 0x00RRGGBB
 * (or 0xAARRGGBB — alpha is ignored). Returns true on success. */
bool gifcap_frame(GifCap *g, const uint32_t *pixels);

/* Persistent memory-mode encoder for streaming single-frame GIFs
 * (e.g. one image per multipart HTTP part). Same scaling semantics as
 * gifcap_open; no file is created. Use with gifcap_encode_single, not
 * gifcap_frame. Returns NULL on failure. */
GifCap *gifcap_open_mem(int in_w, int in_h, int out_w, int out_h,
                        int frame_delay_cs);

/* Encode one frame as a COMPLETE standalone GIF (header + image +
 * trailer, no loop extension) into an internal buffer that is reused by
 * the next call. Only valid on a gifcap_open_mem encoder. Returns a
 * pointer to the encoded bytes (valid until the next call or
 * gifcap_close) and stores the byte count in *out_len; NULL on OOM. */
const uint8_t *gifcap_encode_single(GifCap *g, const uint32_t *pixels,
                                    size_t *out_len);

/* Finalise the file (writes trailer) and free the encoder. Safe with NULL. */
void gifcap_close(GifCap *g);

/* Number of frames written so far (for status display). */
int gifcap_frame_count(const GifCap *g);

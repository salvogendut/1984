/* screen_text — in-memory text-mode screen reader for #129 probe.
 *
 * Each frame, scans CPC video RAM and tries to identify each 8x8 cell
 * against the firmware font table (loaded from the OS ROM at &3800).
 * Produces an 80-column × 25-row character grid for mode 2 (CP/M+
 * default; 80x25 char console) and a 40-column × 25-row grid for
 * mode 1 (BASIC default). When the grid changes from the previous
 * frame, the full grid is streamed out the kbd PTY prefixed by a
 * form-feed marker (\f) so external readers (probe.py) can frame on it.
 *
 * Limitations:
 *   - Mode 3 unsupported; emits an empty grid.
 *   - Graphical apps (SymbOS, GEOS) won't match the firmware font; the
 *     emitted grid will be mostly '?' or blank.
 *   - Apps that redefine the font (TXT.SET.MATRIX, CP/M+ MX4 fonts)
 *     would produce wrong text.
 *   - Cursor blink: an inverted cell would not match a font entry; we
 *     emit '_' as a placeholder.
 *
 * For #129 (CP/M+ console with stock font, mode 2) this is reliable. */
#pragma once
#include "cpc.h"
#include <stdbool.h>

/* Enable/disable the OCR-monitor scan path. Off by default so the
 * per-frame cost is paid only when explicitly requested by the
 * --ocr-monitor CLI flag. */
void screen_text_set_enabled(bool on);
bool screen_text_is_enabled(void);

/* Initialise: extract the firmware font (96 chars × 8 bytes) from the
 * OS ROM at offset 0x3800 and build a u64-keyed lookup. Call once,
 * after the OS ROM has been loaded. */
void screen_text_init(CPC *cpc);

/* Scan video RAM once. If the resulting char grid differs from the
 * previous scan, emit it through kbd_pty_emit_char prefixed by \f and
 * with rows terminated by \r\n. No-op if --kbd-pty wasn't given.
 * Cheap (~2000 byte reads + 8-byte hash lookups per frame) so safe to
 * call once per emulated frame. */
void screen_text_tick(CPC *cpc);

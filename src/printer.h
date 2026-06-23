#pragma once
#include "types.h"
#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* Opaque-pointer forward decls so consumers don't need <cairo.h>. When
 * built without Cairo, these stay NULL. */
typedef struct _cairo cairo_t;
typedef struct _cairo_surface cairo_surface_t;

/*
 * CPC parallel printer port.
 *
 * The CPC presents a 7-bit Centronics-style data port on writes to any
 * I/O address with A12 low (0xEFxx). Bit 7 of the latched byte is the
 * STROBE line, inverted by the hardware: only on strobe-LOW does the
 * byte actually clock into the printer. The PPI's port B bit 6
 * (BUSY input) is hardcoded to "not busy" so guests never throttle.
 *
 * The host-side sink is Cairo → PDF, optionally spooled to the default
 * CUPS printer via `lp` so users can print from a CPC program to a
 * real laser printer. The decoder is intentionally light: ESC eats
 * one operand, printable bytes drop onto a monospace canvas at 10pt,
 * CR/LF/FF/TAB handled, control bytes ignored. Anything more elaborate
 * (graphics, ESC/P sequences) lands as text — the PDF is the captured
 * stream, not a glyph-perfect simulation.
 */

typedef enum {
    PRINT_SINK_PDF = 0,
    PRINT_SINK_REAL,
} PrintSink;

typedef struct Printer {
    bool connected;
    bool pdf_enabled;
    PrintSink sink;

    /* The CPC inverts bit 7 between the data byte and the cable, so a
     * raw Z80 OUT byte with bit 7 SET asserts the strobe (cable LOW).
     * We emit on every write where bit 7 is set — Caprice32's model
     * (cap32.cpp:771-773). No edge detection: real firmware pulses the
     * strobe per byte (assert + deassert) so each char is one emit;
     * homebrew that writes only the asserted byte per char (e.g.
     * direct BASIC `OUT &EFFF, &80+ASC("X")`) also works because each
     * write with bit 7 set prints its data byte. */

    bool pdf_ephemeral;
    char pdf_output_dir[PATH_MAX];
    char pdf_path[PATH_MAX];
    cairo_surface_t *pdf_surface;
    cairo_t *pdf_cr;
    bool pdf_page_open;

    float text_x;
    float text_y;
    int   text_esc_skip;

    /* Idle-flush counter (frames at ~50 Hz). Every print byte resets it
     * to IDLE_FRAMES_TO_FINALISE; printer_tick decrements and closes
     * the PDF when it hits zero so viewers can open the file mid-job. */
    int   idle_countdown;
} Printer;

void printer_init(Printer *p);
void printer_shutdown(Printer *p);
void printer_set_pdf_output_dir(Printer *p, const char *dir);
void printer_set_pdf_enabled(Printer *p, bool enabled);
void printer_set_sink(Printer *p, PrintSink sink);

/* CPC Z80 OUT (0xEFxx),A — passes the full latched byte. The module
 * does the strobe-edge detect and bit-7 invert internally. */
void printer_out(Printer *p, u8 val);

void printer_tick(Printer *p);

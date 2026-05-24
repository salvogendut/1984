#pragma once
#include "types.h"

/*
 * Intel 8255 PPI — Programmable Peripheral Interface
 *
 * Port A (input):  PSG data bus / keyboard row data
 * Port B (input):  cassette data in, CRTC VSYNC, printer busy, hardware sense bits
 * Port C (output): keyboard row select (bits 0-3), PSG control (bits 6-7),
 *                  cassette write / motor (bits 4-5)
 */

typedef struct {
    u8 port_a;         /* current latch */
    u8 port_b;
    u8 port_c;
    u8 control;        /* mode control word */

    u8 kbd_row;        /* keyboard matrix row selected by port C bits 0-3 */
    bool vsync_signal; /* fed from CRTC */
} PPI;

void ppi_init(PPI *p);
void ppi_write(PPI *p, u8 port, u8 val);   /* port 0-3 (A/B/C/ctrl) */
u8   ppi_read(PPI *p, u8 port);
void ppi_set_vsync(PPI *p, bool v);

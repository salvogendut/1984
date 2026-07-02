#pragma once
#include "types.h"
#include "kbd.h"
#include <stdbool.h>

/*
 * AMX mouse — presents on the CPC joystick-1 port (keyboard matrix row 9),
 * NOT an I/O-port device like the SYMBiFACE/Albireo mice.
 *
 * The real AMX interface converts mouse motion into active-LOW pulses on the
 * row-9 direction lines: one pulse per "mickey" of movement. The /joystick1
 * row-select line clocks the pulse generator, so software selects row 9,
 * reads it, then deselects before the next pulse can appear. We reproduce that
 * by delivering exactly one mickey per fresh select-edge to row 9 (see
 * amx_pre_read). Buttons are level-held on the two fire lines.
 *
 * Row 9 bit map (matches src/joy.c):
 *   bit0 UP  bit1 DOWN  bit2 LEFT  bit3 RIGHT  bit4 FIRE1  bit5 FIRE2
 *   bit7 = DEL key — never touched here.
 */

#define AMX_ROW    9
#define AMX_UP     0
#define AMX_DOWN   1
#define AMX_LEFT   2
#define AMX_RIGHT  3
#define AMX_FIRE1  4
#define AMX_FIRE2  5

#define AMX_PENDING_CAP 16   /* cap queued mickeys per direction to bound lag */

typedef struct {
    float acc_x, acc_y;      /* sub-mickey host-pixel remainder */
    int   pending[4];        /* queued mickeys: UP, DOWN, LEFT, RIGHT */
    u8    pulsed_mask;       /* direction bits currently driven low */
    u8    prev_row;          /* last row read — select-edge detection */
} Amx;

void amx_init(Amx *a);
void amx_reset(Amx *a, Keyboard *k);            /* drop pulses + fire bits on row 9 */
void amx_move(Amx *a, int dx, int dy);          /* host pixels -> queued mickeys */
void amx_button(Amx *a, Keyboard *k, int btn, bool down);
void amx_pre_read(Amx *a, Keyboard *k, u8 row); /* select-edge pulse machine */

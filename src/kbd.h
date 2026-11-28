/* 1984 Amstrad CPC emulator
 * Copyright (c) 2026 Salvatore Bognanni
 * Copyright (c) 2026 Nick J. Date
 * Licensed under the GNU General Public License, version 2 (see LICENSE).
 */
#pragma once
#include "types.h"
#include <stdbool.h>

/*
 * CPC keyboard matrix: 10 rows x 8 columns.
 * A row is selected via PPI port C bits 0-3.
 * The PSG reads back the column data on I/O port A.
 * A bit is 0 when the key is pressed (active low).
 */

#define KBD_ROWS 10

typedef struct {
    u8 matrix[KBD_ROWS];   /* one byte per row; bit=0 means key pressed */
} Keyboard;

void kbd_init(Keyboard *k);
void kbd_key_down(Keyboard *k, int row, int col);
void kbd_key_up(Keyboard *k, int row, int col);
u8   kbd_read_row(Keyboard *k, u8 row);

/*
 * Scripted key injection (--key-script). The keyboard analogue of
 * --joy-script: a deterministic way to press CPC keys at given frames, which
 * also works with --load-sna (unlike --paste and --autostart). The script is
 * a comma-separated list of "KEYS:FRAMES" steps held one after another; each
 * frame keyscript_tick() presses the current step's keys and releases the
 * script's other keys.
 *
 * KEYS   = CPC key names joined with '+', or - for no key. Names are
 *          the CPC's own keys, case-insensitive, so a script does not depend
 *          on the host keyboard layout: A-Z, 0-9, F0-F9, FDOT, SPACE, RETURN,
 *          ENTER, ESC, TAB, CAPSLOCK, SHIFT, CTRL, COPY, DEL, CLR, UP, DOWN,
 *          LEFT, RIGHT, CARET, MINUS, AT, LBRACKET, RBRACKET, BACKSLASH,
 *          SEMICOLON, COLON, COMMA, DOT, SLASH.
 * FRAMES = how many emulated frames to hold that step (default 1).
 * e.g. "-:100,G:3,-:50,SHIFT+2:3" = nothing for 100 frames, tap G for 3,
 *      wait 50, then Shift+2 for 3 frames.
 *
 * Only keys named somewhere in the script are driven, so the host keyboard
 * and --paste keep working for every other key. All of them are released on
 * the frame after the last step, so a 1-frame last step is still seen.
 */
#define KEYSCRIPT_MAX_STEPS 256

typedef struct {
    struct { u8 down[KBD_ROWS]; int frames; } step[KEYSCRIPT_MAX_STEPS];
    u8   used[KBD_ROWS];  /* keys named anywhere in the script (bit set) */
    int  nsteps;
    int  cur;             /* current step index */
    int  timer;           /* frames left in the current step */
    bool done;
} KeyScript;

/* Parse spec into ks. Returns false (and leaves ks empty and done) on a parse
 * error, after printing the reason to stderr. */
bool keyscript_init(KeyScript *ks, const char *spec);

/* Call once per frame (next to paste_tick): drives the script's keys. */
void keyscript_tick(KeyScript *ks, Keyboard *k);

/* Map an SDL scancode to (row, col); returns false if unmapped */
#include <SDL3/SDL.h>
bool kbd_sdl_key(Keyboard *k, SDL_Scancode sc, bool pressed);

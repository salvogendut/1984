#pragma once
#include "types.h"

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

/* Map an SDL scancode to (row, col); returns false if unmapped */
#include <SDL2/SDL.h>
bool kbd_sdl_key(Keyboard *k, SDL_Scancode sc, bool pressed);

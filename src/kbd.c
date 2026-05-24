#include "kbd.h"
#include <string.h>

void kbd_init(Keyboard *k) {
    memset(k->matrix, 0xFF, sizeof(k->matrix));   /* all keys up = 0xFF */
}

void kbd_key_down(Keyboard *k, int row, int col) {
    if (row >= 0 && row < KBD_ROWS)
        k->matrix[row] &= ~(1 << col);
}

void kbd_key_up(Keyboard *k, int row, int col) {
    if (row >= 0 && row < KBD_ROWS)
        k->matrix[row] |= (1 << col);
}

u8 kbd_read_row(Keyboard *k, u8 row) {
    if (row < KBD_ROWS) return k->matrix[row];
    return 0xFF;
}

/*
 * CPC keyboard matrix (row, col) for common keys.
 * Source: CPC Hardware Reference / CPCWiki keyboard map.
 */
typedef struct { SDL_Scancode sc; int row, col; } KeyMap;

static const KeyMap keymap[] = {
    /* Row 0 */
    { SDL_SCANCODE_UP,       0, 0 }, { SDL_SCANCODE_RETURN,    0, 2 },
    { SDL_SCANCODE_F9,       0, 1 }, { SDL_SCANCODE_F6,        0, 3 },
    { SDL_SCANCODE_F3,       0, 4 }, { SDL_SCANCODE_KP_ENTER,   0, 5 },
    { SDL_SCANCODE_KP_3,     0, 6 }, { SDL_SCANCODE_KP_6,       0, 7 },
    /* Row 1 */
    { SDL_SCANCODE_DOWN,     1, 0 }, { SDL_SCANCODE_LEFT,       1, 2 },
    { SDL_SCANCODE_RIGHT,    1, 1 }, { SDL_SCANCODE_F7,         1, 3 },
    { SDL_SCANCODE_F8,       1, 4 }, { SDL_SCANCODE_KP_PERIOD,  1, 5 },
    { SDL_SCANCODE_KP_2,     1, 6 }, { SDL_SCANCODE_KP_5,       1, 7 },
    /* Row 2 */
    { SDL_SCANCODE_KP_0,     2, 0 }, { SDL_SCANCODE_KP_COMMA,   2, 1 },
    { SDL_SCANCODE_KP_1,     2, 2 }, { SDL_SCANCODE_F5,         2, 3 },
    { SDL_SCANCODE_F4,       2, 4 }, { SDL_SCANCODE_F2,         2, 5 },
    { SDL_SCANCODE_F1,       2, 6 }, { SDL_SCANCODE_F10,        2, 7 }, /* CPC F0 → PC F10 */
    /* Row 3 */
    { SDL_SCANCODE_BACKSPACE, 3, 0 }, { SDL_SCANCODE_EQUALS,   3, 1 },
    { SDL_SCANCODE_MINUS,     3, 2 }, { SDL_SCANCODE_RIGHTBRACKET, 3, 3 },
    { SDL_SCANCODE_P,         3, 4 }, { SDL_SCANCODE_SEMICOLON,    3, 5 },
    { SDL_SCANCODE_APOSTROPHE,3, 6 }, { SDL_SCANCODE_BACKSLASH,    3, 7 },
    /* Row 4 */
    { SDL_SCANCODE_0,    4, 0 }, { SDL_SCANCODE_9,    4, 1 },
    { SDL_SCANCODE_O,    4, 2 }, { SDL_SCANCODE_I,    4, 3 },
    { SDL_SCANCODE_L,    4, 4 }, { SDL_SCANCODE_K,    4, 5 },
    { SDL_SCANCODE_M,    4, 6 }, { SDL_SCANCODE_COMMA,4, 7 },
    /* Row 5 */
    { SDL_SCANCODE_8,    5, 0 }, { SDL_SCANCODE_7,    5, 1 },
    { SDL_SCANCODE_U,    5, 2 }, { SDL_SCANCODE_Y,    5, 3 },
    { SDL_SCANCODE_H,    5, 4 }, { SDL_SCANCODE_J,    5, 5 },
    { SDL_SCANCODE_N,    5, 6 }, { SDL_SCANCODE_SPACE,5, 7 },
    /* Row 6 */
    { SDL_SCANCODE_6,    6, 0 }, { SDL_SCANCODE_5,    6, 1 },
    { SDL_SCANCODE_R,    6, 2 }, { SDL_SCANCODE_T,    6, 3 },
    { SDL_SCANCODE_G,    6, 4 }, { SDL_SCANCODE_F,    6, 5 },
    { SDL_SCANCODE_B,    6, 6 }, { SDL_SCANCODE_V,    6, 7 },
    /* Row 7 */
    { SDL_SCANCODE_4,    7, 0 }, { SDL_SCANCODE_3,    7, 1 },
    { SDL_SCANCODE_E,    7, 2 }, { SDL_SCANCODE_W,    7, 3 },
    { SDL_SCANCODE_S,    7, 4 }, { SDL_SCANCODE_D,    7, 5 },
    { SDL_SCANCODE_C,    7, 6 }, { SDL_SCANCODE_X,    7, 7 },
    /* Row 8 */
    { SDL_SCANCODE_1,    8, 0 }, { SDL_SCANCODE_2,    8, 1 },
    { SDL_SCANCODE_ESCAPE,8,2 }, { SDL_SCANCODE_Q,    8, 3 },
    { SDL_SCANCODE_TAB,  8, 4 }, { SDL_SCANCODE_A,    8, 5 },
    { SDL_SCANCODE_CAPSLOCK,8,6},{ SDL_SCANCODE_Z,    8, 7 },
    /* Row 9 */
    { SDL_SCANCODE_GRAVE,    9, 0 }, { SDL_SCANCODE_LEFTBRACKET, 9, 1 },
    { SDL_SCANCODE_PERIOD,   9, 2 }, { SDL_SCANCODE_SLASH,       9, 3 },
    { SDL_SCANCODE_LSHIFT,   9, 5 }, { SDL_SCANCODE_RSHIFT,      9, 5 },
    { SDL_SCANCODE_LCTRL,    9, 6 }, { SDL_SCANCODE_RCTRL,       9, 6 },
    { SDL_SCANCODE_LALT,     9, 7 }, /* CPC COPY key */
};

bool kbd_sdl_key(Keyboard *k, SDL_Scancode sc, bool pressed) {
    for (int i = 0; i < (int)(sizeof(keymap)/sizeof(keymap[0])); i++) {
        if (keymap[i].sc == sc) {
            if (pressed)
                kbd_key_down(k, keymap[i].row, keymap[i].col);
            else
                kbd_key_up(k, keymap[i].row, keymap[i].col);
            return true;
        }
    }
    return false;
}

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
 * CPC keyboard matrix mapping. row/col derived from Caprice32 hardware scancodes:
 * row = scancode >> 4, col = physical_bit = scancode & 7.
 */
typedef struct { SDL_Scancode sc; int row, col; } KeyMap;

static const KeyMap keymap[] = {
    /* Row 0: CurUp CurRight CurDown KP9 KP6 KP3 KPEnter KP. */
    { SDL_SCANCODE_UP,        0, 0 },
    { SDL_SCANCODE_RIGHT,     0, 1 },
    { SDL_SCANCODE_DOWN,      0, 2 },
    { SDL_SCANCODE_KP_9,      0, 3 },
    { SDL_SCANCODE_F9,        0, 3 },
    { SDL_SCANCODE_KP_6,      0, 4 },
    { SDL_SCANCODE_F6,        0, 4 },
    { SDL_SCANCODE_KP_3,      0, 5 },
    { SDL_SCANCODE_F3,        0, 5 },
    { SDL_SCANCODE_KP_ENTER,  0, 6 },
    { SDL_SCANCODE_KP_PERIOD, 0, 7 },
    /* Row 1: CurLeft Copy KP7 KP8 KP5 KP1 KP2 KP0 */
    { SDL_SCANCODE_LEFT,      1, 0 },
    { SDL_SCANCODE_LALT,      1, 1 }, /* COPY */
    { SDL_SCANCODE_KP_7,      1, 2 },
    { SDL_SCANCODE_F7,        1, 2 },
    { SDL_SCANCODE_KP_8,      1, 3 },
    { SDL_SCANCODE_F8,        1, 3 },
    { SDL_SCANCODE_KP_5,      1, 4 },
    { SDL_SCANCODE_F5,        1, 4 },
    { SDL_SCANCODE_KP_1,      1, 5 },
    { SDL_SCANCODE_F1,        1, 5 },
    { SDL_SCANCODE_KP_2,      1, 6 },
    { SDL_SCANCODE_F2,        1, 6 },
    { SDL_SCANCODE_KP_0,      1, 7 },
    /* Row 2: CLR [ Return ] KP4 Shift \ Ctrl */
    { SDL_SCANCODE_HOME,         2, 0 }, /* CLR */
    { SDL_SCANCODE_DELETE,       2, 0 }, /* CLR (delete forward → clear line) */
    { SDL_SCANCODE_LEFTBRACKET,  2, 1 },
    { SDL_SCANCODE_RETURN,       2, 2 },
    { SDL_SCANCODE_RIGHTBRACKET, 2, 3 },
    { SDL_SCANCODE_KP_4,         2, 4 },
    { SDL_SCANCODE_F4,           2, 4 },
    { SDL_SCANCODE_LSHIFT,       2, 5 },
    { SDL_SCANCODE_RSHIFT,       2, 5 },
    { SDL_SCANCODE_BACKSLASH,    2, 6 },
    { SDL_SCANCODE_LCTRL,        2, 7 },
    { SDL_SCANCODE_RCTRL,        2, 7 },
    /* Row 3: ^ - @ P ; : / . */
    { SDL_SCANCODE_EQUALS,    3, 0 }, /* ^ on CPC */
    { SDL_SCANCODE_MINUS,     3, 1 },
    { SDL_SCANCODE_GRAVE,     3, 2 }, /* @ on CPC */
    { SDL_SCANCODE_P,         3, 3 },
    { SDL_SCANCODE_SEMICOLON, 3, 4 },
    { SDL_SCANCODE_APOSTROPHE,3, 5 }, /* : on CPC */
    { SDL_SCANCODE_SLASH,     3, 6 },
    { SDL_SCANCODE_PERIOD,    3, 7 },
    /* Row 4: 0 9 O I L K M , */
    { SDL_SCANCODE_0,      4, 0 },
    { SDL_SCANCODE_9,      4, 1 },
    { SDL_SCANCODE_O,      4, 2 },
    { SDL_SCANCODE_I,      4, 3 },
    { SDL_SCANCODE_L,      4, 4 },
    { SDL_SCANCODE_K,      4, 5 },
    { SDL_SCANCODE_M,      4, 6 },
    { SDL_SCANCODE_COMMA,  4, 7 },
    /* Row 5: 8 7 U Y H J N Space */
    { SDL_SCANCODE_8,      5, 0 },
    { SDL_SCANCODE_7,      5, 1 },
    { SDL_SCANCODE_U,      5, 2 },
    { SDL_SCANCODE_Y,      5, 3 },
    { SDL_SCANCODE_H,      5, 4 },
    { SDL_SCANCODE_J,      5, 5 },
    { SDL_SCANCODE_N,      5, 6 },
    { SDL_SCANCODE_SPACE,  5, 7 },
    /* Row 6: 6 5 R T G F B V */
    { SDL_SCANCODE_6,      6, 0 },
    { SDL_SCANCODE_5,      6, 1 },
    { SDL_SCANCODE_R,      6, 2 },
    { SDL_SCANCODE_T,      6, 3 },
    { SDL_SCANCODE_G,      6, 4 },
    { SDL_SCANCODE_F,      6, 5 },
    { SDL_SCANCODE_B,      6, 6 },
    { SDL_SCANCODE_V,      6, 7 },
    /* Row 7: 4 3 E W S D C X */
    { SDL_SCANCODE_4,      7, 0 },
    { SDL_SCANCODE_3,      7, 1 },
    { SDL_SCANCODE_E,      7, 2 },
    { SDL_SCANCODE_W,      7, 3 },
    { SDL_SCANCODE_S,      7, 4 },
    { SDL_SCANCODE_D,      7, 5 },
    { SDL_SCANCODE_C,      7, 6 },
    { SDL_SCANCODE_X,      7, 7 },
    /* Row 8: 1 2 Esc Q Tab A CapsLk Z */
    { SDL_SCANCODE_1,        8, 0 },
    { SDL_SCANCODE_2,        8, 1 },
    { SDL_SCANCODE_ESCAPE,   8, 2 },
    { SDL_SCANCODE_Q,        8, 3 },
    { SDL_SCANCODE_TAB,      8, 4 },
    { SDL_SCANCODE_A,        8, 5 },
    { SDL_SCANCODE_CAPSLOCK, 8, 6 },
    { SDL_SCANCODE_Z,        8, 7 },
    /* Row 9: DEL (backspace) — only real key here; joystick occupies bits 0-5 */
    { SDL_SCANCODE_BACKSPACE, 9, 7 },
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

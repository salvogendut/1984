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
    /* Row 0: F.(0) KPEnter(1) F3(2) F6(3) F9(4) Copy(5) CurRight(6) CurUp(7) */
    { SDL_SCANCODE_KP_PERIOD, 0, 0 },
    { SDL_SCANCODE_KP_ENTER,  0, 1 },
    { SDL_SCANCODE_F3,        0, 2 },
    { SDL_SCANCODE_F6,        0, 3 },
    { SDL_SCANCODE_F9,        0, 4 },
    { SDL_SCANCODE_LALT,      0, 5 }, /* COPY */
    { SDL_SCANCODE_RIGHT,     0, 6 },
    { SDL_SCANCODE_UP,        0, 7 },
    /* Row 1: Del(0) F1(1) F4(2) F7(3) CapsLk(4) Shift(5) CurLeft(6) CurDown(7) */
    { SDL_SCANCODE_BACKSPACE, 1, 0 },
    { SDL_SCANCODE_F1,        1, 1 },
    { SDL_SCANCODE_F4,        1, 2 },
    { SDL_SCANCODE_F7,        1, 3 },
    { SDL_SCANCODE_CAPSLOCK,  1, 4 },
    { SDL_SCANCODE_LSHIFT,    1, 5 },
    { SDL_SCANCODE_RSHIFT,    1, 5 },
    { SDL_SCANCODE_LEFT,      1, 6 },
    { SDL_SCANCODE_DOWN,      1, 7 },
    /* Row 2: Space(0) -(1) Return(2) ^\(3) Ctrl(4) KP→(5) KP←(6) KP↑(7) */
    { SDL_SCANCODE_SPACE,     2, 0 },
    { SDL_SCANCODE_MINUS,     2, 1 },
    { SDL_SCANCODE_RETURN,    2, 2 },
    { SDL_SCANCODE_BACKSLASH, 2, 3 },
    { SDL_SCANCODE_LCTRL,     2, 4 },
    { SDL_SCANCODE_RCTRL,     2, 4 },
    { SDL_SCANCODE_KP_6,      2, 5 },
    { SDL_SCANCODE_KP_4,      2, 6 },
    { SDL_SCANCODE_KP_8,      2, 7 },
    /* Row 3: 3NP(0) 1NP(1) 2NP(2) 5NP(3) 6NP(4) 7NP(5) 0NP(6) .NP(7) */
    { SDL_SCANCODE_KP_3,      3, 0 },
    { SDL_SCANCODE_KP_1,      3, 1 },
    { SDL_SCANCODE_KP_2,      3, 2 },
    { SDL_SCANCODE_KP_5,      3, 3 },
    { SDL_SCANCODE_KP_6,      3, 4 },
    { SDL_SCANCODE_KP_7,      3, 5 },
    { SDL_SCANCODE_KP_0,      3, 6 },
    { SDL_SCANCODE_DELETE,    3, 7 },
    /* Row 4: ,(0) M(1) K(2) L(3) I(4) O(5) 9(6) 0(7) */
    { SDL_SCANCODE_COMMA,  4, 0 },
    { SDL_SCANCODE_M,      4, 1 },
    { SDL_SCANCODE_K,      4, 2 },
    { SDL_SCANCODE_L,      4, 3 },
    { SDL_SCANCODE_I,      4, 4 },
    { SDL_SCANCODE_O,      4, 5 },
    { SDL_SCANCODE_9,      4, 6 },
    { SDL_SCANCODE_0,      4, 7 },
    /* Row 5: N(1) J(2) H(3) Y(4) U(5) 7(6) 8(7) */
    { SDL_SCANCODE_N,      5, 1 },
    { SDL_SCANCODE_J,      5, 2 },
    { SDL_SCANCODE_H,      5, 3 },
    { SDL_SCANCODE_Y,      5, 4 },
    { SDL_SCANCODE_U,      5, 5 },
    { SDL_SCANCODE_7,      5, 6 },
    { SDL_SCANCODE_8,      5, 7 },
    /* Row 6: V(0) B(1) F(2) G(3) T(4) R(5) 5(6) 6(7) */
    { SDL_SCANCODE_V,      6, 0 },
    { SDL_SCANCODE_B,      6, 1 },
    { SDL_SCANCODE_F,      6, 2 },
    { SDL_SCANCODE_G,      6, 3 },
    { SDL_SCANCODE_T,      6, 4 },
    { SDL_SCANCODE_R,      6, 5 },
    { SDL_SCANCODE_5,      6, 6 },
    { SDL_SCANCODE_6,      6, 7 },
    /* Row 7: X(0) C(1) D(2) S(3) W(4) E(5) 3(6) 4(7) */
    { SDL_SCANCODE_X,      7, 0 },
    { SDL_SCANCODE_C,      7, 1 },
    { SDL_SCANCODE_D,      7, 2 },
    { SDL_SCANCODE_S,      7, 3 },
    { SDL_SCANCODE_W,      7, 4 },
    { SDL_SCANCODE_E,      7, 5 },
    { SDL_SCANCODE_3,      7, 6 },
    { SDL_SCANCODE_4,      7, 7 },
    /* Row 8: Z(0) A(2) Tab(3) Q(4) Esc(5) 2(6) 1(7) */
    { SDL_SCANCODE_Z,        8, 0 },
    { SDL_SCANCODE_A,        8, 2 },
    { SDL_SCANCODE_TAB,      8, 3 },
    { SDL_SCANCODE_Q,        8, 4 },
    { SDL_SCANCODE_ESCAPE,   8, 5 },
    { SDL_SCANCODE_2,        8, 6 },
    { SDL_SCANCODE_1,        8, 7 },
    /* Row 9: @(0) [(1) ](2) ;(3) :(4) .(5) ,(6) =(7) -- punctuation */
    { SDL_SCANCODE_GRAVE,        9, 0 }, /* @ on CPC */
    { SDL_SCANCODE_LEFTBRACKET,  9, 1 },
    { SDL_SCANCODE_RIGHTBRACKET, 9, 2 },
    { SDL_SCANCODE_SEMICOLON,    9, 3 },
    { SDL_SCANCODE_APOSTROPHE,   9, 4 }, /* : on CPC */
    { SDL_SCANCODE_PERIOD,       9, 5 },
    { SDL_SCANCODE_SLASH,        9, 6 },
    { SDL_SCANCODE_EQUALS,       9, 7 },
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

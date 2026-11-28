/* 1984 Amstrad CPC emulator
 * Copyright (c) 2026 Salvatore Bognanni
 * Copyright (c) 2026 Nick J. Date
 * Licensed under the GNU General Public License, version 2 (see LICENSE).
 */
#include "kbd.h"
#include <string.h>
#include <stdio.h>

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

/* --- Scripted key injection (--key-script), see kbd.h ---------------------- */

/* CPC key names by matrix position. Row 9 bits 0-5 are joystick 0 (use
 * --joy-script for those); bit 7 is DEL. */
typedef struct { const char *name; int row, col; } KeyName;

static const KeyName keynames[] = {
    { "UP",        0, 0 }, { "RIGHT",     0, 1 }, { "DOWN",      0, 2 },
    { "F9",        0, 3 }, { "F6",        0, 4 }, { "F3",        0, 5 },
    { "ENTER",     0, 6 }, { "FDOT",      0, 7 },
    { "LEFT",      1, 0 }, { "COPY",      1, 1 }, { "F7",        1, 2 },
    { "F8",        1, 3 }, { "F5",        1, 4 }, { "F1",        1, 5 },
    { "F2",        1, 6 }, { "F0",        1, 7 },
    { "CLR",       2, 0 }, { "LBRACKET",  2, 1 }, { "RETURN",    2, 2 },
    { "RBRACKET",  2, 3 }, { "F4",        2, 4 }, { "SHIFT",     2, 5 },
    { "BACKSLASH", 2, 6 }, { "CTRL",      2, 7 },
    { "CARET",     3, 0 }, { "MINUS",     3, 1 }, { "AT",        3, 2 },
    { "P",         3, 3 }, { "SEMICOLON", 3, 4 }, { "COLON",     3, 5 },
    { "SLASH",     3, 6 }, { "DOT",       3, 7 },
    { "0",         4, 0 }, { "9",         4, 1 }, { "O",         4, 2 },
    { "I",         4, 3 }, { "L",         4, 4 }, { "K",         4, 5 },
    { "M",         4, 6 }, { "COMMA",     4, 7 },
    { "8",         5, 0 }, { "7",         5, 1 }, { "U",         5, 2 },
    { "Y",         5, 3 }, { "H",         5, 4 }, { "J",         5, 5 },
    { "N",         5, 6 }, { "SPACE",     5, 7 },
    { "6",         6, 0 }, { "5",         6, 1 }, { "R",         6, 2 },
    { "T",         6, 3 }, { "G",         6, 4 }, { "F",         6, 5 },
    { "B",         6, 6 }, { "V",         6, 7 },
    { "4",         7, 0 }, { "3",         7, 1 }, { "E",         7, 2 },
    { "W",         7, 3 }, { "S",         7, 4 }, { "D",         7, 5 },
    { "C",         7, 6 }, { "X",         7, 7 },
    { "1",         8, 0 }, { "2",         8, 1 }, { "ESC",       8, 2 },
    { "Q",         8, 3 }, { "TAB",       8, 4 }, { "A",         8, 5 },
    { "CAPSLOCK",  8, 6 }, { "Z",         8, 7 },
    { "DEL",       9, 7 },
};

/* Find the key named by the n characters at p (case-insensitive). */
static const KeyName *keyname_find(const char *p, int n) {
    for (int i = 0; i < (int)(sizeof(keynames)/sizeof(keynames[0])); i++) {
        const char *q = keynames[i].name;
        int j = 0;
        while (j < n && q[j]) {
            char c = p[j];
            if (c >= 'a' && c <= 'z') c = (char)(c - 32);
            if (c != q[j]) break;
            j++;
        }
        if (j == n && q[j] == '\0') return &keynames[i];
    }
    return NULL;
}

static bool keyscript_fail(KeyScript *ks, const char *msg, const char *p,
                           int n) {
    fprintf(stderr, "[key-script] %s '%.*s'\n", msg, n, p);
    memset(ks, 0, sizeof(*ks));
    ks->done = true;
    return false;
}

bool keyscript_init(KeyScript *ks, const char *spec) {
    memset(ks, 0, sizeof(*ks));
    const char *p = spec;
    while (*p) {
        if (ks->nsteps >= KEYSCRIPT_MAX_STEPS)
            return keyscript_fail(ks, "too many steps at", p, (int)strlen(p));
        u8 *down = ks->step[ks->nsteps].down;
        /* Key names separated by '+', up to ':' or ','. */
        for (;;) {
            const char *name = p;
            while (*p && *p != '+' && *p != ':' && *p != ',') p++;
            int n = (int)(p - name);
            if (n == 1 && name[0] == '-') {
                /* no key (not 'n' as in --joy-script: N is a key here) */
            } else {
                const KeyName *kn = keyname_find(name, n);
                if (!kn) return keyscript_fail(ks, "unknown key", name, n);
                down[kn->row]     |= (u8)(1 << kn->col);
                ks->used[kn->row] |= (u8)(1 << kn->col);
            }
            if (*p != '+') break;
            p++;
        }
        int frames = 0;
        if (*p == ':') {
            p++;
            while (*p >= '0' && *p <= '9') frames = frames * 10 + (*p++ - '0');
        }
        if (*p && *p != ',')
            return keyscript_fail(ks, "bad step at", p, (int)strlen(p));
        if (frames <= 0) frames = 1;
        ks->step[ks->nsteps].frames = frames;
        ks->nsteps++;
        if (*p == ',') p++;
    }
    ks->cur   = 0;
    ks->timer = ks->nsteps ? ks->step[0].frames : 0;
    ks->done  = (ks->nsteps == 0);
    return !ks->done;
}

void keyscript_tick(KeyScript *ks, Keyboard *k) {
    if (ks->done) return;
    if (ks->cur >= ks->nsteps) {
        /* The frame after the last step: release everything. Doing it here
         * rather than when the last step's timer runs out means the CPC sees
         * that step for every one of its frames, a 1-frame step included. */
        for (int r = 0; r < KBD_ROWS; r++)
            for (int c = 0; c < 8; c++)
                if (ks->used[r] & (1 << c)) kbd_key_up(k, r, c);
        ks->done = true;
        return;
    }
    const u8 *down = ks->step[ks->cur].down;
    for (int r = 0; r < KBD_ROWS; r++)               /* set this step's keys */
        for (int c = 0; c < 8; c++) {
            if (!(ks->used[r] & (1 << c))) continue;
            if (down[r] & (1 << c)) kbd_key_down(k, r, c);
            else                    kbd_key_up  (k, r, c);
        }
    if (--ks->timer <= 0) {                          /* advance to the next */
        ks->cur++;
        if (ks->cur < ks->nsteps)
            ks->timer = ks->step[ks->cur].frames;
    }
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
    { SDL_SCANCODE_APOSTROPHE, 3, 4 }, /* ; on CPC */
    { SDL_SCANCODE_SEMICOLON, 3, 5 }, /* : on CPC */
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

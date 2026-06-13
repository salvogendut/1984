#include "paste.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Shift key position in the CPC matrix */
#define SHIFT_ROW 2
#define SHIFT_COL 5

/* Timing: frames at 50 Hz */
#define HOLD_FRAMES 2   /* how long a key is held */
#define GAP_FRAMES  1   /* silent gap between characters */

/* Optional per-char gap multiplier read from ONE_K_PASTE_GAP at start of
 * paste, e.g. ONE_K_PASTE_GAP=40 will hold a frame for 1 and gap for 40
 * so commands trickle in slowly enough to survive a CP/M boot draining
 * the keyboard buffer between them. 0 means "use defaults". */
static int paste_gap_override = -1;

typedef struct {
    int  row, col;
    bool shift;
    bool valid;
} CpcKey;

/*
 * ASCII 0-127 → CPC matrix position.
 * Unset entries have valid=false and are skipped during injection.
 * Derived from the CPC hardware matrix (same rows/cols as kbd.c).
 */
static const CpcKey keymap[128] = {
    ['\n'] = { 2, 2, false, true },   /* Return */
    [' ']  = { 5, 7, false, true },

    /* Digits — unshifted */
    ['0'] = { 4, 0, false, true },
    ['1'] = { 8, 0, false, true },
    ['2'] = { 8, 1, false, true },
    ['3'] = { 7, 1, false, true },
    ['4'] = { 7, 0, false, true },
    ['5'] = { 6, 1, false, true },
    ['6'] = { 6, 0, false, true },
    ['7'] = { 5, 1, false, true },
    ['8'] = { 5, 0, false, true },
    ['9'] = { 4, 1, false, true },

    /* Lowercase letters */
    ['a'] = { 8, 5, false, true }, ['b'] = { 6, 6, false, true },
    ['c'] = { 7, 6, false, true }, ['d'] = { 7, 5, false, true },
    ['e'] = { 7, 2, false, true }, ['f'] = { 6, 5, false, true },
    ['g'] = { 6, 4, false, true }, ['h'] = { 5, 4, false, true },
    ['i'] = { 4, 3, false, true }, ['j'] = { 5, 5, false, true },
    ['k'] = { 4, 5, false, true }, ['l'] = { 4, 4, false, true },
    ['m'] = { 4, 6, false, true }, ['n'] = { 5, 6, false, true },
    ['o'] = { 4, 2, false, true }, ['p'] = { 3, 3, false, true },
    ['q'] = { 8, 3, false, true }, ['r'] = { 6, 2, false, true },
    ['s'] = { 7, 4, false, true }, ['t'] = { 6, 3, false, true },
    ['u'] = { 5, 2, false, true }, ['v'] = { 6, 7, false, true },
    ['w'] = { 7, 3, false, true }, ['x'] = { 7, 7, false, true },
    ['y'] = { 5, 3, false, true }, ['z'] = { 8, 7, false, true },

    /* Uppercase letters (same key + shift) */
    ['A'] = { 8, 5, true, true }, ['B'] = { 6, 6, true, true },
    ['C'] = { 7, 6, true, true }, ['D'] = { 7, 5, true, true },
    ['E'] = { 7, 2, true, true }, ['F'] = { 6, 5, true, true },
    ['G'] = { 6, 4, true, true }, ['H'] = { 5, 4, true, true },
    ['I'] = { 4, 3, true, true }, ['J'] = { 5, 5, true, true },
    ['K'] = { 4, 5, true, true }, ['L'] = { 4, 4, true, true },
    ['M'] = { 4, 6, true, true }, ['N'] = { 5, 6, true, true },
    ['O'] = { 4, 2, true, true }, ['P'] = { 3, 3, true, true },
    ['Q'] = { 8, 3, true, true }, ['R'] = { 6, 2, true, true },
    ['S'] = { 7, 4, true, true }, ['T'] = { 6, 3, true, true },
    ['U'] = { 5, 2, true, true }, ['V'] = { 6, 7, true, true },
    ['W'] = { 7, 3, true, true }, ['X'] = { 7, 7, true, true },
    ['Y'] = { 5, 3, true, true }, ['Z'] = { 8, 7, true, true },

    /* Symbols — unshifted on CPC */
    ['-']  = { 3, 1, false, true },
    ['@']  = { 3, 2, false, true },
    [';']  = { 3, 4, false, true },
    [':']  = { 3, 5, false, true },
    ['/']  = { 3, 6, false, true },
    ['.']  = { 3, 7, false, true },
    [',']  = { 4, 7, false, true },
    ['[']  = { 2, 1, false, true },
    [']']  = { 2, 3, false, true },
    ['\\'] = { 2, 6, false, true },
    ['^']  = { 3, 0, false, true },

    /* Symbols — require shift on CPC */
    ['!']  = { 8, 0, true, true },   /* shift+1 */
    ['"']  = { 8, 1, true, true },   /* shift+2 */
    ['#']  = { 7, 1, true, true },   /* shift+3 */
    ['$']  = { 7, 0, true, true },   /* shift+4 */
    ['%']  = { 6, 1, true, true },   /* shift+5 */
    ['&']  = { 6, 0, true, true },   /* shift+6 */
    ['\''] = { 5, 1, true, true },   /* shift+7 */
    ['(']  = { 5, 0, true, true },   /* shift+8 */
    [')']  = { 4, 1, true, true },   /* shift+9 */
    ['_']  = { 4, 0, true, true },   /* shift+0 */
    ['=']  = { 3, 1, true, true },   /* shift+- */
    ['+']  = { 3, 4, true, true },   /* shift+; */
    ['*']  = { 3, 5, true, true },   /* shift+: */
    ['?']  = { 3, 6, true, true },   /* shift+/ */
    ['>']  = { 3, 7, true, true },   /* shift+. */
    ['<']  = { 4, 7, true, true },   /* shift+, */
    ['|']  = { 3, 2, true, true },   /* shift+@ */
};

static void key_down(Keyboard *k, const CpcKey *ck) {
    if (ck->shift) kbd_key_down(k, SHIFT_ROW, SHIFT_COL);
    kbd_key_down(k, ck->row, ck->col);
}

static void key_up(Keyboard *k, const CpcKey *ck) {
    kbd_key_up(k, ck->row, ck->col);
    if (ck->shift) kbd_key_up(k, SHIFT_ROW, SHIFT_COL);
}

void paste_init(Paste *p) {
    p->buf   = NULL;
    p->len   = 0;
    p->pos   = 0;
    p->timer = 0;
    p->held  = false;
}

void paste_free(Paste *p) {
    free(p->buf);
    p->buf = NULL;
    p->len = p->pos = 0;
}

void paste_text(Paste *p, const char *text) {
    free(p->buf);
    p->len = (int)strlen(text);
    p->buf = malloc(p->len + 2);  /* +1 for appended newline, +1 for NUL */
    if (!p->buf) { p->len = 0; return; }
    memcpy(p->buf, text, p->len);
    p->buf[p->len++] = '\n';
    p->buf[p->len]   = '\0';
    p->pos   = 0;
    p->timer = 3;   /* wait 3 frames for Ctrl to clear from the CPC matrix */
    p->held  = false;
}

void paste_text_raw(Paste *p, const char *text) {
    free(p->buf);
    p->len = (int)strlen(text);
    p->buf = malloc(p->len + 1);
    if (!p->buf) { p->len = 0; return; }
    memcpy(p->buf, text, p->len);
    p->buf[p->len] = '\0';
    p->pos   = 0;
    p->timer = 3;
    p->held  = false;
}

void paste_tick(Paste *p, Keyboard *k) {
    if (!p->buf || p->pos >= p->len) return;

    if (p->timer > 0) { p->timer--; return; }

    /* Skip \r and unmapped characters */
    while (p->pos < p->len) {
        unsigned char c = (unsigned char)p->buf[p->pos];
        if (c == '\r') { p->pos++; continue; }
        if (c < 128 && keymap[c].valid) break;
        p->pos++;
    }
    if (p->pos >= p->len) return;

    unsigned char c = (unsigned char)p->buf[p->pos];
    const CpcKey *ck = &keymap[c];

    if (!p->held) {
        key_down(k, ck);
        p->held  = true;
        p->timer = HOLD_FRAMES;
    } else {
        key_up(k, ck);
        p->held  = false;
        p->pos++;
        if (paste_gap_override < 0) {
            const char *e = getenv("ONE_K_PASTE_GAP");
            paste_gap_override = e ? atoi(e) : 0;
        }
        p->timer = paste_gap_override > 0 ? paste_gap_override : GAP_FRAMES;
    }
}

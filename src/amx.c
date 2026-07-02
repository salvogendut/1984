#include "amx.h"
#include <string.h>

/* Host pixels per emulated mickey. Relative-mode SDL deltas are much larger
 * than a real mouse's mickey rate, so we divide down. Tunable by feel against
 * real AMX software (AMX Art / Pagemaker). */
#define AMX_DIV 2.0f

void amx_init(Amx *a) {
    memset(a, 0, sizeof(*a));
    a->prev_row = 0xFF;
}

void amx_reset(Amx *a, Keyboard *k) {
    for (int c = AMX_UP; c <= AMX_FIRE2; c++)
        kbd_key_up(k, AMX_ROW, c);
    a->acc_x = a->acc_y = 0.0f;
    a->pulsed_mask = 0;
    a->prev_row = 0xFF;
    for (int i = 0; i < 4; i++) a->pending[i] = 0;
}

static void enqueue(int *slot, int n) {
    *slot += n;
    if (*slot > AMX_PENDING_CAP) *slot = AMX_PENDING_CAP;
}

void amx_move(Amx *a, int dx, int dy) {
    a->acc_x += dx / AMX_DIV;
    a->acc_y += dy / AMX_DIV;
    int mx = (int)a->acc_x;
    int my = (int)a->acc_y;
    a->acc_x -= mx;
    a->acc_y -= my;
    /* SDL y+ is downward, which maps to the CPC joystick DOWN line. */
    if (mx > 0)      enqueue(&a->pending[AMX_RIGHT],  mx);
    else if (mx < 0) enqueue(&a->pending[AMX_LEFT],  -mx);
    if (my > 0)      enqueue(&a->pending[AMX_DOWN],   my);
    else if (my < 0) enqueue(&a->pending[AMX_UP],    -my);
}

void amx_button(Amx *a, Keyboard *k, int btn, bool down) {
    (void)a;
    int col = (btn == 0) ? AMX_FIRE1 : (btn == 1) ? AMX_FIRE2 : -1;
    if (col < 0) return;   /* middle/other buttons unused */
    if (down) kbd_key_down(k, AMX_ROW, col);
    else      kbd_key_up(k, AMX_ROW, col);
}

void amx_pre_read(Amx *a, Keyboard *k, u8 row) {
    if (a->prev_row == AMX_ROW && row != AMX_ROW) {
        /* Deselect: pulsed direction lines return HIGH. */
        for (int d = 0; d < 4; d++)
            if (a->pulsed_mask & (1 << d)) kbd_key_up(k, AMX_ROW, d);
        a->pulsed_mask = 0;
    } else if (row == AMX_ROW && a->prev_row != AMX_ROW) {
        /* Fresh select-edge: clear any stale pulse, then emit one mickey per
         * pending direction (one pulse per select, per hardware). */
        for (int d = 0; d < 4; d++)
            if (a->pulsed_mask & (1 << d)) kbd_key_up(k, AMX_ROW, d);
        a->pulsed_mask = 0;
        for (int d = 0; d < 4; d++) {
            if (a->pending[d] > 0) {
                kbd_key_down(k, AMX_ROW, d);
                a->pending[d]--;
                a->pulsed_mask |= (1 << d);
            }
        }
    }
    a->prev_row = row;
}

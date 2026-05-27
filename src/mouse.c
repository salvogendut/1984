#include "mouse.h"
#include <string.h>

/* Clamp to signed 6-bit range [-32, +31] */
static int clamp6(int v) { return v > 31 ? 31 : v < -32 ? -32 : v; }
/* Clamp to signed 5-bit range [-16, +15] */
static int clamp5(int v) { return v > 15 ? 15 : v < -16 ? -16 : v; }

void mouse_init(Mouse *m) { memset(m, 0, sizeof(*m)); }

/*
 * Protocol (CPCWiki SYMBiFACE II PS/2 mouse):
 *   bits 7:6 = mode (mm), bits 5:0 = data (DDDDDD)
 *   m=00  0x00        no more data — CPC stops reading
 *   m=01  0x40|D      X offset, signed 6-bit, positive = right
 *   m=10  0x80|D      Y offset, signed 6-bit, positive = up
 *   m=11  0xC0|D      D[5]=0 → buttons (bit0=L, bit1=R, bit2=M, bit3=Fwd, bit4=Bck)
 *                     D[5]=1 → scroll, signed 5-bit in D[4:0]
 *
 * Only packets for data that actually changed are emitted. CPC reads until 0x00.
 */
u8 mouse_read(Mouse *m) {
    /* Start of burst: snapshot accumulated state, clear accumulators */
    if (m->state == 0) {
        m->snap_dx  = m->dx;  m->dx = 0;
        m->snap_dy  = m->dy;  m->dy = 0;
        m->snap_dz  = m->dz;  m->dz = 0;
        m->snap_btns = (m->btn[0] ? 0x01u : 0u)   /* left   bit0 */
                     | (m->btn[1] ? 0x02u : 0u)   /* right  bit1 */
                     | (m->btn[2] ? 0x04u : 0u);  /* middle bit2 */
        m->snap_btn_changed = m->btn_changed;
        m->btn_changed = false;
        m->state = 1;
    }

    /* Walk through slots; skip empty ones, return first non-empty */
    for (;;) {
        switch (m->state) {
        case 1:
            m->state = 2;
            if (m->snap_dx != 0)
                return (u8)(0x40u | ((u8)clamp6(m->snap_dx) & 0x3Fu));
            break;
        case 2:
            m->state = 3;
            /* SDL Y positive = down; protocol positive = up → negate */
            if (m->snap_dy != 0)
                return (u8)(0x80u | ((u8)clamp6(-m->snap_dy) & 0x3Fu));
            break;
        case 3:
            m->state = 4;
            if (m->snap_btn_changed)
                return (u8)(0xC0u | m->snap_btns);
            break;
        case 4:
            m->state = 0;
            if (m->snap_dz != 0)
                return (u8)(0xE0u | ((u8)clamp5(-m->snap_dz) & 0x1Fu));
            break;
        default:
            m->state = 0;
            /* fall through */
        case 0:
            return 0x00u;
        }
    }
}

void mouse_move(Mouse *m, int dx, int dy) {
    m->dx += dx;
    m->dy += dy;
}

void mouse_scroll(Mouse *m, int dz) {
    m->dz += dz;
}

void mouse_button(Mouse *m, int btn, bool pressed) {
    if (btn >= 0 && btn < 3) {
        m->btn[btn] = pressed;
        m->btn_changed = true;
    }
}

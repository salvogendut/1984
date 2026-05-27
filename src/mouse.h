#pragma once
#include "types.h"
#include <stdbool.h>

typedef struct {
    /* Accumulated deltas from SDL events */
    int  dx, dy, dz;
    bool btn[3];            /* 0=left, 1=right, 2=middle */
    bool btn_changed;       /* button state changed since last report */
    /* Snapshot taken at start of each read burst */
    int  snap_dx, snap_dy, snap_dz;
    u8   snap_btns;
    bool snap_btn_changed;
    /* 0=idle (next read starts burst), 1=X, 2=Y, 3=buttons, 4=scroll */
    int  state;
} Mouse;

void mouse_init(Mouse *m);
u8   mouse_read(Mouse *m);   /* called on port 0xFD10 read */
void mouse_move(Mouse *m, int dx, int dy);
void mouse_scroll(Mouse *m, int dz);
void mouse_button(Mouse *m, int btn, bool pressed);

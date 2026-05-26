#pragma once
#include <SDL3/SDL.h>
#include "kbd.h"

/*
 * Gamepad / joystick support.
 * All connected gamepads are treated as CPC joystick 1, which lives in
 * keyboard matrix row 9 bits 0-5 (Up Down Left Right Fire1 Fire2).
 */

#define JOY_MAX_PADS 4

typedef struct {
    SDL_Gamepad *pad[JOY_MAX_PADS];
    int          count;
} Joy;

void joy_init(Joy *j);
void joy_destroy(Joy *j);

/* Returns true if the event was a joystick event (caller should not re-process it). */
bool joy_handle_event(Joy *j, const SDL_Event *ev, Keyboard *k);

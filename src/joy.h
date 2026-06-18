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
    SDL_Gamepad  *pad[JOY_MAX_PADS];
    int           count;
    /* Raw joystick fallback for devices not in the SDL gamepad database */
    SDL_Joystick *raw[JOY_MAX_PADS];
    int           raw_count;
} Joy;

void joy_init(Joy *j);
void joy_destroy(Joy *j);

/* Returns true if the event was a joystick event (caller should not re-process it). */
bool joy_handle_event(Joy *j, const SDL_Event *ev, Keyboard *k);

/*
 * Scripted joystick injection (--joy-script). A deterministic, headless way to
 * drive the CPC joystick (row 9) for automated UI tests - the joystick analogue
 * of --paste. The script is a comma-separated list of "DIRS:FRAMES" steps held
 * one after another; each frame joyscript_tick() sets row 9 to the current step.
 *
 * DIRS  = any of u d l r (directions), 1 2 (Fire1/Fire2), or - / n for neutral.
 * FRAMES = how many emulated frames to hold that state (default 1).
 * e.g. "d:150,-:30,u:150,-:30,1:5" = down 150f, rest, up 150f, rest, tap Fire1.
 */
#define JOYSCRIPT_MAX_STEPS 128

typedef struct {
    struct { unsigned char mask; int frames; } step[JOYSCRIPT_MAX_STEPS];
    int  nsteps;
    int  cur;       /* current step index */
    int  timer;     /* frames left in the current step */
    bool done;
} JoyScript;

/* Parse spec into js. Returns false (and leaves js empty/done) on a parse error. */
bool joyscript_init(JoyScript *js, const char *spec);

/* Call once per frame (next to paste_tick): drives row 9 from the script. */
void joyscript_tick(JoyScript *js, Keyboard *k);

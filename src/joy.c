#include "joy.h"
#include "cpc.h"
#include <string.h>
#include <stdio.h>

/* CPC joystick 1: keyboard matrix row 9, bits 0-5 */
#define JOY_ROW   9
#define JOY_UP    0
#define JOY_DOWN  1
#define JOY_LEFT  2
#define JOY_RIGHT 3
#define JOY_FIRE1 4
#define JOY_FIRE2 5

#define AXIS_DEAD 8000   /* ±8000 out of ±32767 */

void joy_init(Joy *j) {
    memset(j, 0, sizeof(*j));

    int count = 0;
    SDL_JoystickID *ids = SDL_GetJoysticks(&count);
    fprintf(stderr, "joy: %d device(s) found at startup\n", count);
    if (!ids) return;

    for (int i = 0; i < count && (j->count + j->raw_count) < JOY_MAX_PADS; i++) {
        if (SDL_IsGamepad(ids[i])) {
            SDL_Gamepad *g = SDL_OpenGamepad(ids[i]);
            if (g && j->count < JOY_MAX_PADS) {
                fprintf(stderr, "joy: opened gamepad %d: %s\n", ids[i],
                        SDL_GetGamepadName(g));
                j->pad[j->count++] = g;
            }
        } else {
            SDL_Joystick *r = SDL_OpenJoystick(ids[i]);
            if (r && j->raw_count < JOY_MAX_PADS) {
                fprintf(stderr, "joy: opened raw joystick %d: %s\n", ids[i],
                        SDL_GetJoystickName(r));
                j->raw[j->raw_count++] = r;
            }
        }
    }
    SDL_free(ids);
}

void joy_destroy(Joy *j) {
    for (int i = 0; i < j->count; i++)     SDL_CloseGamepad(j->pad[i]);
    for (int i = 0; i < j->raw_count; i++) SDL_CloseJoystick(j->raw[i]);
}

static void axis_update(Keyboard *k, int neg_col, int pos_col, int val) {
    if (val < -AXIS_DEAD) {
        kbd_key_down(k, JOY_ROW, neg_col);
        kbd_key_up  (k, JOY_ROW, pos_col);
    } else if (val > AXIS_DEAD) {
        kbd_key_up  (k, JOY_ROW, neg_col);
        kbd_key_down(k, JOY_ROW, pos_col);
    } else {
        kbd_key_up(k, JOY_ROW, neg_col);
        kbd_key_up(k, JOY_ROW, pos_col);
    }
}

bool joy_handle_event(Joy *j, const SDL_Event *ev, Keyboard *k) {
    switch (ev->type) {

    /* ---- Gamepad (database-mapped devices) ---- */

    case SDL_EVENT_GAMEPAD_ADDED: {
        if (cpc_trace_input)
            fprintf(stderr, "[input] SDL_EVENT_GAMEPAD_ADDED which=%d (already count=%d)\n",
                    ev->gdevice.which, j->count);
        if (j->count < JOY_MAX_PADS) {
            SDL_Gamepad *g = SDL_OpenGamepad(ev->gdevice.which);
            if (g) j->pad[j->count++] = g;
        }
        return true;
    }

    case SDL_EVENT_GAMEPAD_REMOVED: {
        for (int i = 0; i < j->count; i++) {
            if (SDL_GetGamepadID(j->pad[i]) == ev->gdevice.which) {
                SDL_CloseGamepad(j->pad[i]);
                j->pad[i] = j->pad[--j->count];
                j->pad[j->count] = NULL;
                break;
            }
        }
        for (int c = 0; c <= JOY_FIRE2; c++)
            kbd_key_up(k, JOY_ROW, c);
        return true;
    }

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool pressed = (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
        if (cpc_trace_input)
            fprintf(stderr, "[input] gamepad button %d %s\n",
                    ev->gbutton.button, pressed ? "DOWN" : "UP");
        int col = -1;
        switch ((SDL_GamepadButton)ev->gbutton.button) {
        case SDL_GAMEPAD_BUTTON_DPAD_UP:    col = JOY_UP;    break;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  col = JOY_DOWN;  break;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  col = JOY_LEFT;  break;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: col = JOY_RIGHT; break;
        case SDL_GAMEPAD_BUTTON_SOUTH:      col = JOY_FIRE1; break;
        case SDL_GAMEPAD_BUTTON_EAST:
        case SDL_GAMEPAD_BUTTON_WEST:
        case SDL_GAMEPAD_BUTTON_NORTH:      col = JOY_FIRE2; break;
        default: break;
        }
        if (col >= 0) {
            if (pressed) kbd_key_down(k, JOY_ROW, col);
            else         kbd_key_up  (k, JOY_ROW, col);
            return true;
        }
        break;
    }

    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (cpc_trace_input)
            fprintf(stderr, "[input] gamepad axis %d = %d\n",
                    ev->gaxis.axis, ev->gaxis.value);
        switch ((SDL_GamepadAxis)ev->gaxis.axis) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            axis_update(k, JOY_LEFT, JOY_RIGHT, ev->gaxis.value);
            return true;
        case SDL_GAMEPAD_AXIS_LEFTY:
            axis_update(k, JOY_UP, JOY_DOWN, ev->gaxis.value);
            return true;
        default:
            break;
        }
        break;
    }

    /* ---- Raw joystick fallback (devices not in gamepad database) ---- */

    case SDL_EVENT_JOYSTICK_ADDED: {
        /* Only open as raw if SDL doesn't recognise it as a gamepad */
        if (!SDL_IsGamepad(ev->jdevice.which) && j->raw_count < JOY_MAX_PADS) {
            SDL_Joystick *r = SDL_OpenJoystick(ev->jdevice.which);
            if (r) j->raw[j->raw_count++] = r;
        }
        return true;
    }

    case SDL_EVENT_JOYSTICK_REMOVED: {
        for (int i = 0; i < j->raw_count; i++) {
            if (SDL_GetJoystickID(j->raw[i]) == ev->jdevice.which) {
                SDL_CloseJoystick(j->raw[i]);
                j->raw[i] = j->raw[--j->raw_count];
                j->raw[j->raw_count] = NULL;
                break;
            }
        }
        for (int c = 0; c <= JOY_FIRE2; c++)
            kbd_key_up(k, JOY_ROW, c);
        return true;
    }

    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP: {
        /* Only handle if it belongs to one of our raw (non-gamepad) devices */
        bool is_raw = false;
        for (int i = 0; i < j->raw_count; i++) {
            if (SDL_GetJoystickID(j->raw[i]) == ev->jbutton.which) {
                is_raw = true; break;
            }
        }
        if (!is_raw) break;
        bool pressed = (ev->type == SDL_EVENT_JOYSTICK_BUTTON_DOWN);
        int col = -1;
        switch (ev->jbutton.button) {
        case 0: col = JOY_FIRE1; break;
        case 1: col = JOY_FIRE2; break;
        default: break;
        }
        if (col >= 0) {
            if (pressed) kbd_key_down(k, JOY_ROW, col);
            else         kbd_key_up  (k, JOY_ROW, col);
            return true;
        }
        break;
    }

    case SDL_EVENT_JOYSTICK_AXIS_MOTION: {
        bool is_raw = false;
        for (int i = 0; i < j->raw_count; i++) {
            if (SDL_GetJoystickID(j->raw[i]) == ev->jaxis.which) {
                is_raw = true; break;
            }
        }
        if (!is_raw) break;
        switch (ev->jaxis.axis) {
        case 0:
            if (cpc_trace_input) fprintf(stderr, "[input] raw joy axis0=%d\n", ev->jaxis.value);
            axis_update(k, JOY_LEFT, JOY_RIGHT, ev->jaxis.value); return true;
        case 1:
            if (cpc_trace_input) fprintf(stderr, "[input] raw joy axis1=%d\n", ev->jaxis.value);
            axis_update(k, JOY_UP,   JOY_DOWN,  ev->jaxis.value); return true;
        default: break;
        }
        break;
    }

    case SDL_EVENT_JOYSTICK_HAT_MOTION: {
        bool is_raw = false;
        for (int i = 0; i < j->raw_count; i++) {
            if (SDL_GetJoystickID(j->raw[i]) == ev->jhat.which) {
                is_raw = true; break;
            }
        }
        if (!is_raw) break;
        Uint8 hat = ev->jhat.value;
        if (hat & SDL_HAT_UP)    kbd_key_down(k, JOY_ROW, JOY_UP);
        else                     kbd_key_up  (k, JOY_ROW, JOY_UP);
        if (hat & SDL_HAT_DOWN)  kbd_key_down(k, JOY_ROW, JOY_DOWN);
        else                     kbd_key_up  (k, JOY_ROW, JOY_DOWN);
        if (hat & SDL_HAT_LEFT)  kbd_key_down(k, JOY_ROW, JOY_LEFT);
        else                     kbd_key_up  (k, JOY_ROW, JOY_LEFT);
        if (hat & SDL_HAT_RIGHT) kbd_key_down(k, JOY_ROW, JOY_RIGHT);
        else                     kbd_key_up  (k, JOY_ROW, JOY_RIGHT);
        return true;
    }

    default:
        break;
    }
    return false;
}

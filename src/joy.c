#include "joy.h"
#include <string.h>

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
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (ids) {
        for (int i = 0; i < count && j->count < JOY_MAX_PADS; i++) {
            SDL_Gamepad *g = SDL_OpenGamepad(ids[i]);
            if (g) j->pad[j->count++] = g;
        }
        SDL_free(ids);
    }
}

void joy_destroy(Joy *j) {
    for (int i = 0; i < j->count; i++)
        SDL_CloseGamepad(j->pad[i]);
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

    case SDL_EVENT_GAMEPAD_ADDED: {
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
        /* Release all joystick bits so nothing stays stuck */
        for (int c = 0; c <= JOY_FIRE2; c++)
            kbd_key_up(k, JOY_ROW, c);
        return true;
    }

    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        bool pressed = (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
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

    default:
        break;
    }
    return false;
}

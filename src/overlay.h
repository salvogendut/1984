#pragma once
#include <stdbool.h>
#include <SDL3/SDL.h>
#include "config.h"

typedef enum {
    OV_GENERAL  = 0,
    OV_STORAGE  = 1,
    OV_ADVANCED = 2,
    OV_SEC_COUNT = 3
} OvSection;

typedef struct {
    bool      visible;
    OvSection section;
    int       row;
    Config   *cfg;
} Overlay;

void overlay_init(Overlay *ov, Config *cfg);

/* Returns true if the event was consumed by the overlay. */
bool overlay_handle_event(Overlay *ov, SDL_Event *ev);

/* Draw the overlay on top of the current renderer frame (before display_flip). */
void overlay_render(const Overlay *ov, SDL_Renderer *r);

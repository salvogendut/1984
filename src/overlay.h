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

typedef enum {
    OV_STATE_MENU    = 0,   /* normal navigation */
    OV_STATE_CONFIRM = 1    /* "save changes?" prompt */
} OvState;

typedef struct {
    bool         visible;
    OvSection    section;
    int          row;
    OvState      state;
    bool         dirty;        /* any unsaved changes since overlay opened */
    Config      *cfg;
    Config       saved;        /* snapshot taken when overlay opens */
    CPC         *cpc;          /* for live disk operations */
    /* pending file-dialog result */
    int          dialog_drive; /* 0=A, 1=B, -1=none */
    char         dialog_path[512];
    bool         dialog_ready;
} Overlay;

void overlay_init(Overlay *ov, Config *cfg, CPC *cpc);

/* Returns true if the event was consumed by the overlay. */
bool overlay_handle_event(Overlay *ov, SDL_Event *ev);

/* Draw the overlay on top of the current renderer frame (before display_flip). */
void overlay_render(const Overlay *ov, SDL_Renderer *r);

/* Call once per frame to process any pending file-dialog results. */
void overlay_tick(Overlay *ov);

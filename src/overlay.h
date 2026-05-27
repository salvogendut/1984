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
    OV_STATE_MENU     = 0,   /* normal navigation */
    OV_STATE_CONFIRM  = 1,   /* "save changes?" prompt */
    OV_STATE_ROMSLOTS = 2    /* ROM slots sub-panel */
} OvState;

typedef enum {
    DIALOG_NONE      = 0,
    DIALOG_DISK      = 1,
    DIALOG_ROMSLOT   = 2,
    DIALOG_LOWER_ROM = 3
} DialogKind;

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
    DialogKind   dialog_kind;
    int          dialog_drive; /* 0=A, 1=B (DIALOG_DISK) */
    int          dialog_slot;  /* 0-31     (DIALOG_ROMSLOT) */
    char         dialog_path[512];
    bool         dialog_ready;
    /* ROM slots sub-panel state */
    int          romslot_row;    /* selected slot 0-31 */
    int          romslot_scroll; /* index of first visible slot */
    /* set by overlay after a save that requires a cold boot */
    bool         needs_cold_boot;
} Overlay;

void overlay_init(Overlay *ov, Config *cfg, CPC *cpc);

/* Returns true if the event was consumed by the overlay. */
bool overlay_handle_event(Overlay *ov, SDL_Event *ev);

/* Draw the overlay on top of the current renderer frame (before display_flip). */
void overlay_render(const Overlay *ov, SDL_Renderer *r);

/* Call once per frame to process any pending file-dialog results. */
void overlay_tick(Overlay *ov);

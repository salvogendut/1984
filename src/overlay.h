#pragma once
#include <stdbool.h>
#include <SDL3/SDL.h>
#include "config.h"

typedef enum {
    OV_GENERAL  = 0,
    OV_STORAGE  = 1,
    OV_ADVANCED = 2,
    OV_TINKER   = 3,
    OV_SEC_COUNT = 4
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
    DIALOG_LOWER_ROM = 3,
    DIALOG_IDE       = 4,
    DIALOG_M4_IMAGE  = 5,
    DIALOG_ALBIREO   = 6,
    DIALOG_BASIC_ROM = 7,
    DIALOG_TAPE      = 8,
    DIALOG_SNAPSHOT_LOAD = 9,
    DIALOG_SNAPSHOT_SAVE = 10
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
    /* Inline editor for the boards CSV (Ins key on a populated slot). */
    bool         romslot_editing;
    char         romslot_edit_buf[64];
    int          romslot_edit_len;
    /* set by overlay after a save that requires a cold boot */
    bool         needs_cold_boot;
    /* Last-seen state of the three ROM-owning hardware toggles.
     * Initialised in overlay_init from the loaded config. After every
     * event AND every dialog-callback tick, the overlay diffs the
     * current config against this snapshot; if any of the three flipped
     * (Enter-on-toggle, file-dialog completion, anywhere) we re-apply
     * board ROM templates and flag a cold boot. This catches the
     * async dialog path that direct hooks in handle_event miss. */
    bool         last_m4;
    bool         last_albireo;
    bool         last_symbiface_ide;
} Overlay;

void overlay_init(Overlay *ov, Config *cfg, CPC *cpc);

/* Returns true if the event was consumed by the overlay. */
bool overlay_handle_event(Overlay *ov, SDL_Event *ev);

/* Draw the overlay on top of the current renderer frame (before display_flip). */
void overlay_render(const Overlay *ov, SDL_Renderer *r);

/* Call once per frame to process any pending file-dialog results. */
void overlay_tick(Overlay *ov);

#pragma once
#include <stdbool.h>
#include <SDL3/SDL.h>
#include "config.h"
#include "disk.h"

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
    OV_STATE_ROMSLOTS = 2,   /* ROM slots sub-panel */
    OV_STATE_FILE_BROWSER = 3,
    OV_STATE_REAL_TAPE = 4,  /* Tinker-gated physical cassette controls */
    OV_STATE_ABOUT    = 5,   /* About dialog with an OK button */
    OV_STATE_DISK_AUTOSTART = 6
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
    DIALOG_SNAPSHOT_SAVE = 10,
    DIALOG_VIDEO_CAPTURE = 11,
    DIALOG_PRINTER_DIR   = 12,
    DIALOG_DISK_NEW      = 13,  /* save-as: create a blank .dsk */
    DIALOG_REAL_TAPE_OUTPUT_FILE = 14,
    DIALOG_REAL_TAPE_SOURCE_WAV = 15,
    DIALOG_REAL_TAPE_OUTPUT_CDT = 16,
    DIALOG_CARTRIDGE = 17
} DialogKind;

struct OverlayBrowserEntry;

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
    bool         dialog_failed;
    char         dialog_error[256];
    /* SDL-rendered DSK browser. Forced by --sdl-fm, available with
     * Shift+Enter, and used when the platform dialog reports an error. */
    bool         sdl_fm;
    int          browser_drive;
    char         browser_dir[CONFIG_PATH_MAX];
    char         browser_error[256];
    struct OverlayBrowserEntry *browser_entries;
    int          browser_entry_count;
    int          browser_entry_capacity;
    int          browser_row;
    int          browser_scroll;
    /* Session-only DSK autostart selector. The committed choice survives
     * machine resets but is deliberately absent from Config. */
    DiskDirectoryEntry disk_files[DISK_DIRECTORY_MAX_FILES];
    int          disk_file_count;
    int          disk_file_row;
    int          disk_file_scroll;
    int          disk_file_drive;
    int          disk_file_marked_row;
    bool         disk_file_remember;
    int          disk_autostart_drive;
    uint8_t      disk_autostart_user;
    char         disk_autostart_file[DISK_AMSDOS_NAME_MAX];
    bool         disk_autostart_request;
    int          real_tape_row;
    /* ROM slots sub-panel state */
    int          romslot_row;    /* selected slot 0-31 */
    int          romslot_scroll; /* index of first visible slot */
    /* Inline editor for the boards CSV (Ins key on a populated slot). */
    bool         romslot_editing;
    char         romslot_edit_buf[64];
    int          romslot_edit_len;
    /* Inline editor for the USIfAC PTY link path (Advanced row). When
     * active, TEXT_INPUT events feed pty_link_edit_buf; Enter commits,
     * Esc cancels, Backspace deletes the last byte, Delete clears the
     * buffer. SDL_StartTextInput is owned by handle_event. */
    bool         pty_link_editing;
    char         pty_link_edit_buf[CONFIG_PATH_MAX];
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

void overlay_init(Overlay *ov, Config *cfg, CPC *cpc, bool sdl_fm);
void overlay_quit(Overlay *ov);

/* Returns true if the event was consumed by the overlay. */
bool overlay_handle_event(Overlay *ov, SDL_Event *ev);

/* Draw the overlay on top of the current renderer frame (before display_flip). */
void overlay_render(const Overlay *ov, SDL_Renderer *r);
void overlay_render_real_tape_scope(const Overlay *ov, SDL_Renderer *r);

/* Call once per frame to process any pending file-dialog results. */
void overlay_tick(Overlay *ov);

/* Query the session-only DSK autostart choice. The request accessor consumes
 * only the immediate reset request; the selected file remains armed. */
bool overlay_disk_autostart_get(const Overlay *ov, int *drive,
                                uint8_t *user, const char **file);
bool overlay_take_disk_autostart_request(Overlay *ov);

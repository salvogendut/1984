#include "overlay.h"
#include "cpc.h"
#include "amx.h"
#include "tap.h"     /* TAP_SUPPORTED */
#include "m4.h"
#include "disk.h"
#include "mem.h"
#include "snapshot.h"
#include "webmcap.h"   /* WEBMCAP_SUPPORTED */
#include "ffmpeg_gif.h" /* FFMPEG_GIF_SUPPORTED */
#include "webgui.h"
#include "leds.h"
#include "notify.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/stat.h>

static void overlay_file_callback(void *userdata, const char * const *files, int filter);

/* ---- Layout constants (logical pixels at 1.5× render scale) ---- */
#define SCALE     1.5f
#define FONT_W    SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE   /* 8 logical = 12 screen */
#define FONT_H    SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE   /* 8 logical = 12 screen */
#define BAR_H     27   /* logical ≈ 40 screen px */
#define ITEM_H    20   /* logical = 30 screen px per dropdown row */
#define DROP_PAD   6   /* left margin inside dropdown */
#define VAL_X    140   /* x where values start */
/* ROM slots panel: idx 0 = Lower ROM, idx 1-32 = upper slots 0-31 */
#define ROMSLOT_VISIBLE 10
#define ROMSLOT_TOTAL   (ROM_EXT_COUNT + 1)
#define BROWSER_VISIBLE_ROWS 16
#define BROWSER_ROW_H 16

typedef struct OverlayBrowserEntry {
    char *name;
    bool  directory;
    bool  parent;
} OverlayBrowserEntry;

static const char *const sec_labels[OV_SEC_COUNT] = {
    "General", "Media", "Extensions", "Advanced"
};
static const int sec_x[OV_SEC_COUNT] = { 8, 80, 160, 248 };
/* General has 8 rows on CPC 464, 9 on 6128 (the extra one is the
 * "External Tape" toggle, only meaningful on the 6128 since the 464 has
 * the cassette deck built in). Other sections are fixed.
 * The Advanced tab (OV_TINKER) is hidden unless cfg->tinker is enabled. */
static const int sec_row_count[OV_SEC_COUNT] = { 8, 3, 14, 20 };

static int ov_section_rows(const Overlay *ov, OvSection s) {
    if (s == OV_GENERAL && ov->cfg->model != MODEL_464) return 9;
    if (s == OV_TINKER && ov->cfg->real_crt) return sec_row_count[s] + 6;
    return sec_row_count[s];
}

static int tinker_logical_row(const Overlay *ov, int row) {
    if (!ov->cfg->real_crt)
        return row;
    if (row == 2) return -1;  /* Scanlines */
    if (row == 3) return -2;  /* Brightness */
    if (row == 4) return -3;  /* Contrast */
    if (row == 5) return -4;  /* Red */
    if (row == 6) return -5;  /* Green */
    if (row == 7) return -6;  /* Blue */
    if (row >= 8) return row - 6;
    return row;
}

static void overlay_apply_crt(Overlay *ov) {
    if (!ov->cpc)
        return;
    display_set_crt(&ov->cpc->display, ov->cfg->real_crt,
                    ov->cfg->crt_scanlines, ov->cfg->crt_brightness,
                    ov->cfg->crt_contrast, ov->cfg->crt_red,
                    ov->cfg->crt_green, ov->cfg->crt_blue);
}

static int cycle_crt_percent(int v, int lo, int hi) {
    v += 5;
    if (v > hi)
        v = lo;
    return v;
}

static int cycle_gif_width(int width) {
    switch (width) {
    case 768: return 576;
    case 576: return 384;
    case 384: return 256;
    case 256: return 192;
    default:  return 768;
    }
}

static int cycle_gif_fps(int fps) {
    switch (fps) {
    case 25: return 20;
    case 20: return 10;
    case 10: return 5;
    default: return 25;
    }
}

static bool reset_tinker_item(Overlay *ov) {
    if (ov->section != OV_TINKER)
        return false;

    bool old_real_crt = ov->cfg->real_crt;
    int old_scanlines = ov->cfg->crt_scanlines;
    int old_brightness = ov->cfg->crt_brightness;
    int old_contrast = ov->cfg->crt_contrast;
    int old_red = ov->cfg->crt_red;
    int old_green = ov->cfg->crt_green;
    int old_blue = ov->cfg->crt_blue;
    int old_gif_width = ov->cfg->gif_width;
    int old_gif_fps = ov->cfg->gif_fps;
    bool old_gif_ffmpeg = ov->cfg->gif_ffmpeg;

    switch (tinker_logical_row(ov, ov->row)) {
    case -6:
        ov->cfg->crt_blue = DISPLAY_CRT_RGB_DEFAULT;
        break;
    case -5:
        ov->cfg->crt_green = DISPLAY_CRT_RGB_DEFAULT;
        break;
    case -4:
        ov->cfg->crt_red = DISPLAY_CRT_RGB_DEFAULT;
        break;
    case -3:
        ov->cfg->crt_contrast = DISPLAY_CRT_CONTRAST_DEFAULT;
        break;
    case -2:
        ov->cfg->crt_brightness = DISPLAY_CRT_BRIGHTNESS_DEFAULT;
        break;
    case -1:
        ov->cfg->crt_scanlines = DISPLAY_CRT_SCANLINES_DEFAULT;
        break;
    case 1:
        ov->cfg->real_crt = false;
        ov->cfg->crt_scanlines = DISPLAY_CRT_SCANLINES_DEFAULT;
        ov->cfg->crt_brightness = DISPLAY_CRT_BRIGHTNESS_DEFAULT;
        ov->cfg->crt_contrast = DISPLAY_CRT_CONTRAST_DEFAULT;
        ov->cfg->crt_red = DISPLAY_CRT_RGB_DEFAULT;
        ov->cfg->crt_green = DISPLAY_CRT_RGB_DEFAULT;
        ov->cfg->crt_blue = DISPLAY_CRT_RGB_DEFAULT;
        break;
    case 8:
        ov->cfg->gif_width = GIF_CAPTURE_WIDTH_DEFAULT;
        break;
    case 9:
        ov->cfg->gif_fps = GIF_CAPTURE_FPS_DEFAULT;
        break;
    case 10:
        ov->cfg->gif_ffmpeg = false;
        break;
    default:
        return false;
    }

    bool crt_changed = old_real_crt != ov->cfg->real_crt ||
                       old_scanlines != ov->cfg->crt_scanlines ||
                       old_brightness != ov->cfg->crt_brightness ||
                       old_contrast != ov->cfg->crt_contrast ||
                       old_red != ov->cfg->crt_red ||
                       old_green != ov->cfg->crt_green ||
                       old_blue != ov->cfg->crt_blue;
    bool changed = crt_changed || old_gif_width != ov->cfg->gif_width ||
                   old_gif_fps != ov->cfg->gif_fps ||
                   old_gif_ffmpeg != ov->cfg->gif_ffmpeg;
    if (changed) {
        if (crt_changed)
            overlay_apply_crt(ov);
        ov->dirty = true;
    }
    return true;
}

/* ---- Drawing helpers ---- */

static void fill_rect(SDL_Renderer *r, float x, float y, float w, float h,
                      Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
    SDL_SetRenderDrawBlendMode(r, A < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, R, G, B, A);
    SDL_FRect rect = { x, y, w, h };
    SDL_RenderFillRect(r, &rect);
}

static void draw_rect_outline(SDL_Renderer *r, float x, float y, float w, float h,
                              Uint8 R, Uint8 G, Uint8 B) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, R, G, B, 255);
    SDL_FRect rect = { x, y, w, h };
    SDL_RenderRect(r, &rect);
}

static void draw_text(SDL_Renderer *r, float x, float y, const char *s,
                      Uint8 R, Uint8 G, Uint8 B) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, R, G, B, 255);
    SDL_RenderDebugText(r, x, y, s);
}

static void trunc_path(const char *path, char *out, size_t sz) {
    size_t len = strlen(path);
    if (len < sz) {
        snprintf(out, sz, "%s", path);
    } else {
        size_t keep = sz - 4;
        snprintf(out, sz, "...%s", path + len - keep);
    }
}

/* True when floppy drives are accessible (664/6128 always; 464 only with DD1) */
static bool floppy_accessible(const Overlay *ov) {
    return ov->cfg->model != MODEL_464 || ov->cfg->dd1;
}

static bool path_separator(char c) {
    return c == '/' || c == '\\';
}

static void copy_dirname(char *dst, size_t cap, const char *path) {
    if (!path || !path[0] || cap == 0) return;
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
    if (!slash) return;

    size_t n = (size_t)(slash - path);
    if (n == 0) n = 1;
    if (n == 2 && path[1] == ':') n = 3;
    if (n >= cap) n = cap - 1;
    memcpy(dst, path, n);
    dst[n] = '\0';
}

static bool dsk_filename(const char *name) {
    const char *dot = name ? strrchr(name, '.') : NULL;
    return dot && SDL_strcasecmp(dot, ".dsk") == 0;
}

static void browser_clear_entries(Overlay *ov) {
    for (int i = 0; i < ov->browser_entry_count; i++)
        SDL_free(ov->browser_entries[i].name);
    SDL_free(ov->browser_entries);
    ov->browser_entries = NULL;
    ov->browser_entry_count = 0;
    ov->browser_entry_capacity = 0;
    ov->browser_row = 0;
    ov->browser_scroll = 0;
}

static bool browser_add_entry(Overlay *ov, const char *name,
                              bool directory, bool parent) {
    if (ov->browser_entry_count == ov->browser_entry_capacity) {
        int next = ov->browser_entry_capacity
                 ? ov->browser_entry_capacity * 2 : 32;
        OverlayBrowserEntry *entries = SDL_realloc(
            ov->browser_entries, (size_t)next * sizeof(*entries));
        if (!entries) {
            SDL_SetError("Out of memory while listing disk images");
            return false;
        }
        ov->browser_entries = entries;
        ov->browser_entry_capacity = next;
    }

    char *copy = SDL_strdup(name);
    if (!copy) {
        SDL_SetError("Out of memory while listing disk images");
        return false;
    }
    OverlayBrowserEntry *entry =
        &ov->browser_entries[ov->browser_entry_count++];
    entry->name = copy;
    entry->directory = directory;
    entry->parent = parent;
    return true;
}

static bool join_path(char *dst, size_t cap, const char *dir,
                      const char *name) {
    size_t len = strlen(dir);
    const char *sep = (len > 0 && path_separator(dir[len - 1])) ? "" : "/";
    int written = snprintf(dst, cap, "%s%s%s", dir, sep, name);
    return written >= 0 && (size_t)written < cap;
}

static SDL_EnumerationResult SDLCALL browser_enum_entry(
    void *userdata, const char *dirname, const char *fname) {
    Overlay *ov = userdata;
    char path[CONFIG_PATH_MAX];
    SDL_PathInfo info;

    if (!fname[0] || strcmp(fname, ".") == 0 || strcmp(fname, "..") == 0)
        return SDL_ENUM_CONTINUE;
    int written = snprintf(path, sizeof(path), "%s%s", dirname, fname);
    if (written < 0 || written >= (int)sizeof(path))
        return SDL_ENUM_CONTINUE;
    if (!SDL_GetPathInfo(path, &info))
        return SDL_ENUM_CONTINUE;

    bool directory = info.type == SDL_PATHTYPE_DIRECTORY;
    if (!directory && (info.type != SDL_PATHTYPE_FILE || !dsk_filename(fname)))
        return SDL_ENUM_CONTINUE;
    return browser_add_entry(ov, fname, directory, false)
         ? SDL_ENUM_CONTINUE : SDL_ENUM_FAILURE;
}

static int browser_entry_compare(const void *a, const void *b) {
    const OverlayBrowserEntry *ea = a;
    const OverlayBrowserEntry *eb = b;
    if (ea->parent != eb->parent) return ea->parent ? -1 : 1;
    if (ea->directory != eb->directory) return ea->directory ? -1 : 1;
    return SDL_strcasecmp(ea->name, eb->name);
}

static void normalize_directory(char *path) {
    size_t len = strlen(path);
    while (len > 1 && path_separator(path[len - 1])) {
        if (len == 3 && path[1] == ':') break;
        path[--len] = '\0';
    }
}

static bool parent_directory(const char *dir, char *parent, size_t cap) {
    if (!dir || !dir[0] || cap == 0) return false;
    snprintf(parent, cap, "%s", dir);
    normalize_directory(parent);
    size_t len = strlen(parent);
    size_t slash = len;
    while (slash > 0 && !path_separator(parent[slash - 1])) slash--;
    if (slash == 0) return false;

    size_t keep = slash - 1;
    if (keep == 0) keep = 1;
    if (keep == 2 && parent[1] == ':') keep = 3;
    parent[keep] = '\0';
    return strcmp(parent, dir) != 0;
}

static bool browser_set_directory(Overlay *ov, const char *dir) {
    SDL_PathInfo info;
    char normalized[CONFIG_PATH_MAX];
    char parent[CONFIG_PATH_MAX];

    if (!dir || !dir[0]) return false;
    snprintf(normalized, sizeof(normalized), "%s", dir);
    normalize_directory(normalized);
    if (!SDL_GetPathInfo(normalized, &info)
        || info.type != SDL_PATHTYPE_DIRECTORY)
        return false;

    browser_clear_entries(ov);
    snprintf(ov->browser_dir, sizeof(ov->browser_dir), "%s", normalized);
    ov->browser_error[0] = '\0';

    if (parent_directory(normalized, parent, sizeof(parent))
        && !browser_add_entry(ov, "..", true, true)) {
        snprintf(ov->browser_error, sizeof(ov->browser_error), "%s",
                 SDL_GetError());
        return true;
    }

    if (!SDL_EnumerateDirectory(normalized, browser_enum_entry, ov)) {
        snprintf(ov->browser_error, sizeof(ov->browser_error), "%s",
                 SDL_GetError());
    }
    if (ov->browser_entry_count > 1) {
        qsort(ov->browser_entries, (size_t)ov->browser_entry_count,
              sizeof(*ov->browser_entries), browser_entry_compare);
    }
    return true;
}

static void open_internal_disk_browser(Overlay *ov, int drive) {
    char seed[CONFIG_PATH_MAX] = "";
    const char *mounted = drive == 0 ? ov->cfg->disk_a : ov->cfg->disk_b;
    copy_dirname(seed, sizeof(seed), mounted);

    ov->browser_drive = drive;
    ov->browser_error[0] = '\0';
    ov->dialog_kind = DIALOG_NONE;
    ov->dialog_drive = -1;
    ov->dialog_ready = false;
    ov->dialog_failed = false;

    bool opened = seed[0] && browser_set_directory(ov, seed);
    if (!opened && ov->cfg->last_dir[0])
        opened = browser_set_directory(ov, ov->cfg->last_dir);

    char *cwd = NULL;
    if (!opened) {
        cwd = SDL_GetCurrentDirectory();
        if (cwd) opened = browser_set_directory(ov, cwd);
    }
    SDL_free(cwd);

    if (!opened) {
        browser_clear_entries(ov);
        snprintf(ov->browser_dir, sizeof(ov->browser_dir), ".");
        snprintf(ov->browser_error, sizeof(ov->browser_error),
                 "Cannot read the current directory: %s", SDL_GetError());
    }
    ov->state = OV_STATE_FILE_BROWSER;
}

static void open_disk_dialog(Overlay *ov, int drive) {
    if (ov->sdl_fm) {
        open_internal_disk_browser(ov, drive);
        return;
    }

    ov->dialog_kind = DIALOG_DISK;
    ov->dialog_drive = drive;
    ov->dialog_ready = false;
    ov->dialog_failed = false;
    ov->dialog_error[0] = '\0';
    static const SDL_DialogFileFilter filters[] = {
        { "DSK images", "dsk;DSK" },
        { "All files",  "*"       },
    };
    SDL_ShowOpenFileDialog(overlay_file_callback, ov,
        ov->cpc ? ov->cpc->display.window : NULL,
        filters, 2, ov->cfg->last_dir[0] ? ov->cfg->last_dir : NULL, false);
}

static void browser_ensure_visible(Overlay *ov) {
    if (ov->browser_row < ov->browser_scroll)
        ov->browser_scroll = ov->browser_row;
    if (ov->browser_row >= ov->browser_scroll + BROWSER_VISIBLE_ROWS)
        ov->browser_scroll = ov->browser_row - BROWSER_VISIBLE_ROWS + 1;
    if (ov->browser_scroll < 0) ov->browser_scroll = 0;
}

static void browser_move(Overlay *ov, int delta) {
    if (ov->browser_entry_count == 0) return;
    int row = ov->browser_row + delta;
    if (row < 0) row = 0;
    if (row >= ov->browser_entry_count)
        row = ov->browser_entry_count - 1;
    ov->browser_row = row;
    browser_ensure_visible(ov);
}

static void browser_open_parent(Overlay *ov) {
    char parent[CONFIG_PATH_MAX];
    if (parent_directory(ov->browser_dir, parent, sizeof(parent)))
        browser_set_directory(ov, parent);
}

static void browser_activate_entry(Overlay *ov, bool directories_only) {
    if (ov->browser_row < 0
        || ov->browser_row >= ov->browser_entry_count)
        return;

    OverlayBrowserEntry *entry = &ov->browser_entries[ov->browser_row];
    if (entry->parent) {
        browser_open_parent(ov);
        return;
    }

    char path[CONFIG_PATH_MAX];
    if (!join_path(path, sizeof(path), ov->browser_dir, entry->name)) {
        snprintf(ov->browser_error, sizeof(ov->browser_error),
                 "Path is too long");
        return;
    }
    if (entry->directory) {
        if (!browser_set_directory(ov, path)) {
            snprintf(ov->browser_error, sizeof(ov->browser_error),
                     "Cannot open directory: %.220s", entry->name);
        }
        return;
    }
    if (directories_only) return;

    snprintf(ov->dialog_path, sizeof(ov->dialog_path), "%s", path);
    ov->dialog_kind = DIALOG_DISK;
    ov->dialog_drive = ov->browser_drive;
    ov->state = OV_STATE_MENU;
    browser_clear_entries(ov);
    SDL_MemoryBarrierRelease();
    ov->dialog_ready = true;
}

static void browser_handle_key(Overlay *ov, SDL_Keycode key) {
    switch (key) {
    case SDLK_ESCAPE:
        browser_clear_entries(ov);
        ov->state = OV_STATE_MENU;
        break;
    case SDLK_UP:
        browser_move(ov, -1);
        break;
    case SDLK_DOWN:
        browser_move(ov, 1);
        break;
    case SDLK_PAGEUP:
        browser_move(ov, -BROWSER_VISIBLE_ROWS);
        break;
    case SDLK_PAGEDOWN:
        browser_move(ov, BROWSER_VISIBLE_ROWS);
        break;
    case SDLK_HOME:
        ov->browser_row = 0;
        browser_ensure_visible(ov);
        break;
    case SDLK_END:
        if (ov->browser_entry_count > 0)
            ov->browser_row = ov->browser_entry_count - 1;
        browser_ensure_visible(ov);
        break;
    case SDLK_LEFT:
    case SDLK_BACKSPACE:
        browser_open_parent(ov);
        break;
    case SDLK_RIGHT:
        browser_activate_entry(ov, true);
        break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE:
        browser_activate_entry(ov, false);
        break;
    default:
        break;
    }
}

/* ---- Menu item text ---- */

static void item_text(const Overlay *ov, int row,
                      char *lbl, size_t lsz,
                      char *val, size_t vsz,
                      bool *readonly) {
    *readonly = false;

    switch (ov->section) {

    case OV_GENERAL: {
        bool has_external_tape_row = (ov->cfg->model != MODEL_464);
        /* On disk machines (664/6128) the rows are: Model, Memory, MX4,
         * Roms Board, External Tape, OS ROM, BASIC ROM, Tinker. On 464 the
         * External Tape row is hidden (464 has a built-in deck — always
         * available); subsequent rows shift up by one. */
        int logical = row;
        if (!has_external_tape_row && row >= 4) logical = row + 1;
        switch (logical) {
        case 0:
            snprintf(lbl, lsz, "Model");
            snprintf(val, vsz, "%s",
                ov->cfg->model == MODEL_464 ? "CPC 464" :
                (ov->cfg->model == MODEL_664 ? "CPC 664" : "CPC 6128"));
            break;
        case 1:
            snprintf(lbl, lsz, "Memory");
            snprintf(val, vsz, "%d KB", ov->cfg->memory_kb);
            break;
        case 2:
            snprintf(lbl, lsz, "MX4");
            snprintf(val, vsz, "%s", ov->cfg->mx4 ? "enabled" : "disabled");
            break;
        case 3:
            snprintf(lbl, lsz, "Roms Board");
            snprintf(val, vsz, "%s", ov->cfg->rom_board ? "enabled" : "disabled");
            break;
        case 4:
            snprintf(lbl, lsz, "External Tape");
            snprintf(val, vsz, "%s", ov->cfg->external_tape ? "enabled" : "disabled");
            break;
        case 5: {
            char tmp[CONFIG_PATH_MAX];
            snprintf(lbl, lsz, "OS ROM");
            snprintf(tmp, sizeof(tmp), "%s", ov->cfg->rom_os);
            trunc_path(basename(tmp), val, vsz);
            break;
        }
        case 6: {
            char tmp[CONFIG_PATH_MAX];
            snprintf(lbl, lsz, "BASIC ROM");
            snprintf(tmp, sizeof(tmp), "%s", ov->cfg->rom_basic);
            trunc_path(basename(tmp), val, vsz);
            break;
        }
        case 7:
            snprintf(lbl, lsz, "Tinker");
            snprintf(val, vsz, "%s", ov->cfg->tinker ? "enabled" : "disabled");
            break;
        case 8:
            snprintf(lbl, lsz, "Fallback Input");
            snprintf(val, vsz, "%s",
                ov->cfg->fallback_input == FALLBACK_AMX_MOUSE ? "AMX Mouse" : "Joystick");
            break;
        }
        break;
    }

    case OV_STORAGE: {
        bool accessible = floppy_accessible(ov);
        Disk *da = ov->cpc ? &ov->cpc->drive[0] : NULL;
        Disk *db = ov->cpc ? &ov->cpc->drive[1] : NULL;
        switch (row) {
        case 0:
            snprintf(lbl, lsz, "Drive A");
            if (!accessible) {
                snprintf(val, vsz, "[enable DD1 in Advanced]");
                *readonly = true;
            } else if (da && da->inserted && ov->cfg->disk_a[0]) {
                trunc_path(ov->cfg->disk_a, val, vsz);
            }
            else
                snprintf(val, vsz, "[empty]  Enter=load, N=new, Del=clear");
            break;
        case 1:
            snprintf(lbl, lsz, "Drive B");
            if (!accessible) {
                snprintf(val, vsz, "[enable DD1 in Advanced]");
                *readonly = true;
            } else if (db && db->inserted && ov->cfg->disk_b[0]) {
                trunc_path(ov->cfg->disk_b, val, vsz);
            }
            else
                snprintf(val, vsz, "[empty]  Enter=load, N=new, Del=clear");
            break;
        case 2:
            snprintf(lbl, lsz, "Tape");
            if (ov->cfg->tape[0]) {
                char tmp[CONFIG_PATH_MAX];
                snprintf(tmp, sizeof(tmp), "%s", ov->cfg->tape);
                trunc_path(basename(tmp), val, vsz);
            } else {
                snprintf(val, vsz, "[empty]  Enter=load");
            }
            break;
        }
        break;
    }

    case OV_ADVANCED:
        switch (row) {
        case 0:
            snprintf(lbl, lsz, "M4");
            if (!ov->cfg->mx4) {
                snprintf(val, vsz, "[needs MX4]");
                *readonly = true;
            } else if (ov->cfg->m4 && ov->cfg->m4_image[0]) {
                char tmp[CONFIG_PATH_MAX];
                snprintf(tmp, sizeof(tmp), "%s", ov->cfg->m4_image);
                trunc_path(basename(tmp), val, vsz);
            } else {
                snprintf(val, vsz, "%s", ov->cfg->m4 ? "enabled" : "disabled");
            }
            break;
        case 1:
            snprintf(lbl, lsz, "USIfAC RS232");
            if (ov->cfg->usifac) {
                if (!strcmp(ov->cfg->usifac_backend, "tcp"))
                    snprintf(val, vsz, "TCP:%d", ov->cfg->usifac_tcp_port);
                else if (ov->cpc && ov->cpc->usifac.pty_slave[0])
                    snprintf(val, vsz, "PTY:%s", ov->cpc->usifac.pty_slave);
                else
                    snprintf(val, vsz, "enabled");
            } else {
                snprintf(val, vsz, "disabled");
            }
            break;
        case 2:
            snprintf(lbl, lsz, "Net4CPC");
            snprintf(val, vsz, "%s", ov->cfg->net4cpc ? "enabled" : "disabled");
            break;
        case 3:
            snprintf(lbl, lsz, "RTC");
            snprintf(val, vsz, "%s", ov->cfg->rtc ? "enabled" : "disabled");
            break;
        case 4:
            snprintf(lbl, lsz, "DD1");
            if (ov->cfg->model != MODEL_464) {
                snprintf(val, vsz, "N/A (built-in FDC)");
                *readonly = true;
            } else {
                snprintf(val, vsz, "%s", ov->cfg->dd1 ? "enabled" : "disabled");
            }
            break;
        case 5:
            snprintf(lbl, lsz, "ROM Slots");
            if (!ov->cfg->rom_board)
                snprintf(val, vsz, "[disabled — see General \xbb Roms Board]");
            else
                snprintf(val, vsz, "Enter to configure \xbb");
            *readonly = true;
            break;
        case 6: {
            char diag[CONFIG_PATH_MAX];
            config_default_diag(diag, sizeof(diag));
            bool available = (access(diag, R_OK) == 0);
            snprintf(lbl, lsz, "Diag Cart");
            if (!available) {
                snprintf(val, vsz, "[ROM not found]");
                *readonly = true;
            } else {
                bool active = strcmp(ov->cfg->rom_os, diag) == 0;
                snprintf(val, vsz, "%s", active ? "ON" : "OFF");
            }
            break;
        }
        case 7:
            snprintf(lbl, lsz, "SYMBiFACE IDE");
            if (!ov->cfg->mx4) {
                snprintf(val, vsz, "[needs MX4]");
                *readonly = true;
            } else if (ov->cfg->symbiface_ide && ov->cfg->ide_image[0]) {
                char tmp[CONFIG_PATH_MAX];
                snprintf(tmp, sizeof(tmp), "%s", ov->cfg->ide_image);
                trunc_path(basename(tmp), val, vsz);
            } else {
                snprintf(val, vsz, "%s", ov->cfg->symbiface_ide ? "enabled" : "disabled");
            }
            break;
        case 8:
            snprintf(lbl, lsz, "SYMBiFACE Mouse");
            if (!ov->cfg->mx4) {
                snprintf(val, vsz, "[needs MX4]");
                *readonly = true;
            } else {
                snprintf(val, vsz, "%s", ov->cfg->symbiface_mouse ? "enabled" : "disabled");
            }
            break;
        case 9:
            snprintf(lbl, lsz, "CH376-A Mouse");
            if (!ov->cfg->mx4) {
                snprintf(val, vsz, "(Albireo compatible) [needs MX4]");
                *readonly = true;
            } else if (!ov->cfg->albireo) {
                snprintf(val, vsz, "(Albireo compatible) [needs CH376-B Disk]");
                *readonly = true;
            } else {
                snprintf(val, vsz, "(Albireo compatible) %s",
                         ov->cfg->albireo_mouse ? "enabled" : "disabled");
            }
            break;
        case 10:
            snprintf(lbl, lsz, "CH376-B Disk");
            if (!ov->cfg->mx4) {
                snprintf(val, vsz, "(Albireo compatible) [needs MX4]");
                *readonly = true;
            } else if (ov->cfg->albireo && ov->cfg->albireo_image[0]) {
                char tmp[CONFIG_PATH_MAX];
                char base[24];
                snprintf(tmp, sizeof(tmp), "%s", ov->cfg->albireo_image);
                trunc_path(basename(tmp), base, sizeof(base));
                snprintf(val, vsz, "(Albireo compatible) %s", base);
            } else {
                snprintf(val, vsz, "(Albireo compatible) %s",
                         ov->cfg->albireo ? "enabled" : "disabled");
            }
            break;
        case 11: {
            snprintf(lbl, lsz, "Cyboard");
            if (!ov->cfg->mx4) {
                snprintf(val, vsz, "[needs MX4]");
                *readonly = true;
            } else {
                bool all = ov->cfg->net4cpc && ov->cfg->rtc &&
                           ov->cfg->symbiface_ide && ov->cfg->symbiface_mouse;
                bool none = !ov->cfg->net4cpc && !ov->cfg->rtc &&
                            !ov->cfg->symbiface_ide && !ov->cfg->symbiface_mouse;
                snprintf(val, vsz, "%s", all ? "enabled" : none ? "disabled" : "partial");
            }
            break;
        }
        case 12:
            snprintf(lbl, lsz, "Printer");
            if (!ov->cfg->mx4) {
                snprintf(val, vsz, "[needs MX4]");
                *readonly = true;
            } else if (!ov->cfg->pdf_printer) {
                snprintf(val, vsz, "off  Enter=on");
            } else if (ov->cfg->pdf_printer_dir[0]) {
                char tmp[CONFIG_PATH_MAX];
                snprintf(tmp, sizeof(tmp), "%s", ov->cfg->pdf_printer_dir);
                trunc_path(tmp, val, vsz);
            } else {
                snprintf(val, vsz, "on (no dir)");
            }
            break;
        case 13:
            snprintf(lbl, lsz, "Wi-Fi Modem");
            if (!ov->cfg->mx4) {
                snprintf(val, vsz, "[needs MX4]");
                *readonly = true;
            } else if (!ov->cfg->usifac) {
                snprintf(val, vsz, "[needs USIfAC RS232]");
                *readonly = true;
            } else if (ov->cfg->perryfi && ov->cpc
                       && ov->cpc->perryfi.state == PERRYFI_STATE_ONLINE
                       && ov->cpc->perryfi.remote_host[0]) {
                snprintf(val, vsz, "online %s:%d",
                         ov->cpc->perryfi.remote_host,
                         ov->cpc->perryfi.remote_port);
            } else {
                snprintf(val, vsz, "%s",
                         ov->cfg->perryfi ? "enabled" : "disabled");
            }
            break;
        }
        break;

    case OV_TINKER:
        switch (tinker_logical_row(ov, row)) {
        case -6:
            snprintf(lbl, lsz, "Blue");
            snprintf(val, vsz, "%d%%", ov->cfg->crt_blue);
            break;
        case -5:
            snprintf(lbl, lsz, "Green");
            snprintf(val, vsz, "%d%%", ov->cfg->crt_green);
            break;
        case -4:
            snprintf(lbl, lsz, "Red");
            snprintf(val, vsz, "%d%%", ov->cfg->crt_red);
            break;
        case -3:
            snprintf(lbl, lsz, "Contrast");
            snprintf(val, vsz, "%d%%", ov->cfg->crt_contrast);
            break;
        case -2:
            snprintf(lbl, lsz, "Brightness");
            snprintf(val, vsz, "%d%%", ov->cfg->crt_brightness);
            break;
        case -1:
            snprintf(lbl, lsz, "Scanlines");
            snprintf(val, vsz, "%d%%", ov->cfg->crt_scanlines);
            break;
        case 0:
            snprintf(lbl, lsz, "Smoothing");
            snprintf(val, vsz, "%s",
                     ov->cfg->fullscreen_smoothing ? "smooth" : "sharp");
            break;
        case 1:
            snprintf(lbl, lsz, "Real CRT");
            snprintf(val, vsz, "%s", ov->cfg->real_crt ? "enabled" : "disabled");
            break;
        case 2:
            snprintf(lbl, lsz, "Load Snapshot");
            snprintf(val, vsz, "[Enter to pick .sna]");
            *readonly = true;
            break;
        case 3:
            snprintf(lbl, lsz, "Save Snapshot");
            snprintf(val, vsz, "[Enter to save .sna]");
            *readonly = true;
            break;
        case 4:
            snprintf(lbl, lsz, "Net4CPC TAP");
            if (!TAP_SUPPORTED) {
                snprintf(val, vsz, "[unsupported on this OS]");
                *readonly = true;
            } else if (!ov->cfg->net4cpc) {
                snprintf(val, vsz, "[needs Net4CPC]");
                *readonly = true;
            } else {
                snprintf(val, vsz, "%s",
                         ov->cfg->net4cpc_tap ? "enabled (auto-setup)" : "disabled");
            }
            break;
        case 5:
            snprintf(lbl, lsz, "DHCP server");
            if (!TAP_SUPPORTED) {
                snprintf(val, vsz, "[Net4CPC TAP unsupported]");
            } else if (!ov->cfg->net4cpc || !ov->cfg->net4cpc_tap) {
                snprintf(val, vsz, "[Net4CPC TAP off]");
            } else {
                snprintf(val, vsz, "%s, lease %s-%s",
                         ov->cfg->net4cpc_tap_host_ip,
                         ov->cfg->net4cpc_tap_lease_start,
                         ov->cfg->net4cpc_tap_lease_end);
            }
            *readonly = true;   /* see NET4CPC.md; edit values in 1984.conf */
            break;
        case 6:
            snprintf(lbl, lsz, "Debugging");
            snprintf(val, vsz, "%s",
                     ov->cfg->debug ? "enabled" : "disabled");
            break;
        case 7:
            snprintf(lbl, lsz, "Capture video");
            if (!WEBMCAP_SUPPORTED) {
                snprintf(val, vsz, "[needs ffmpeg — F6 still records .gif]");
            } else if (videocap_active()) {
                snprintf(val, vsz, "recording (%d frames) — Enter to stop",
                         videocap_frame_count());
            } else {
                snprintf(val, vsz, "[Enter to pick .webm]");
            }
            *readonly = true;
            break;
        case 8:
            snprintf(lbl, lsz, "GIF resolution");
            snprintf(val, vsz, "%dx%d", ov->cfg->gif_width,
                     (ov->cfg->gif_width * 3) / 4);
            break;
        case 9:
            snprintf(lbl, lsz, "GIF frame rate");
            snprintf(val, vsz, "%d fps", ov->cfg->gif_fps);
            break;
        case 10:
            snprintf(lbl, lsz, "GIF encoder");
            if (!FFMPEG_GIF_SUPPORTED) {
                snprintf(val, vsz, "built-in [ffmpeg unavailable]");
                *readonly = true;
            } else {
                snprintf(val, vsz, "%s",
                         ov->cfg->gif_ffmpeg ? "FFmpeg optimize" : "built-in");
            }
            break;
        case 11:
            snprintf(lbl, lsz, "USIfAC mode");
            if (!ov->cfg->usifac) {
                snprintf(val, vsz, "[USIfAC RS232 disabled]");
                *readonly = true;
            } else if (!strcmp(ov->cfg->usifac_backend, "tcp")) {
                snprintf(val, vsz, "TCP:%d", ov->cfg->usifac_tcp_port);
            } else {
                snprintf(val, vsz, "PTY");
            }
            break;
        case 12:
            snprintf(lbl, lsz, "USIfAC PTY link");
            if (!ov->cfg->usifac) {
                snprintf(val, vsz, "[USIfAC RS232 disabled]");
                *readonly = true;
            } else if (strcmp(ov->cfg->usifac_backend, "pty")) {
                snprintf(val, vsz, "[PTY backend off]");
                *readonly = true;
            } else if (ov->cfg->usifac_pty_link[0]) {
                snprintf(val, vsz, "%s", ov->cfg->usifac_pty_link);
            } else {
                snprintf(val, vsz, "[Enter to pick path]");
                *readonly = true;
            }
            break;
        case 13:
            snprintf(lbl, lsz, "Monochrome");
            switch (ov->cfg->monochrome) {
                case MONO_GREEN: snprintf(val, vsz, "green"); break;
                case MONO_AMBER: snprintf(val, vsz, "amber"); break;
                case MONO_WHITE: snprintf(val, vsz, "white"); break;
                default:         snprintf(val, vsz, "off");   break;
            }
            break;
        case 14:
            snprintf(lbl, lsz, "Printer mode");
            if (!ov->cfg->mx4) {
                snprintf(val, vsz, "[needs MX4]");
                *readonly = true;
            } else {
                snprintf(val, vsz, "%s",
                         ov->cfg->print_sink == PRINTER_SINK_REAL_PRINTER
                             ? "Real printer (CUPS lp)" : "PDF file");
            }
            break;
        case 15:
            snprintf(lbl, lsz, "Volume");
            snprintf(val, vsz, "%d%%", ov->cfg->audio_volume);
            break;
        case 16:
            snprintf(lbl, lsz, "Stereo");
            if (ov->cfg->audio_stereo_sep == 0)
                snprintf(val, vsz, "mono");
            else if (ov->cfg->audio_stereo_sep == 255)
                snprintf(val, vsz, "ABC (full)");
            else
                snprintf(val, vsz, "ABC %d/255", ov->cfg->audio_stereo_sep);
            break;
        case 17:
            snprintf(lbl, lsz, "Notifications");
            snprintf(val, vsz, "%s",
                     ov->cfg->notifications == NOTIFY_MODE_SCREEN  ? "screen"  :
                     ov->cfg->notifications == NOTIFY_MODE_CONSOLE ? "console" : "off");
            break;
        case 18:
            snprintf(lbl, lsz, "Web GUI");
            if (webgui_active())
                snprintf(val, vsz, "on - 0.0.0.0:%d", webgui_port());
            else
                snprintf(val, vsz, "off (port %d)", ov->cfg->web_port);
            break;
        case 19:
            snprintf(lbl, lsz, "Version");
            snprintf(val, vsz, "%s (commit %s)", PACKAGE_VERSION, PROG_GIT_COMMIT);
            *readonly = true;
            break;
        }
        break;

    default:
        break;
    }
}

/* ---- Value cycling — marks dirty, does NOT save ---- */

/* Turn off the AMX fallback mouse (General tab). Used when an MX4 pointer
 * mouse is enabled in Extensions — the two contend for the host pointer, so
 * only one can be active. Pure input routing, no cold boot needed. */
static void amx_release_pointer(Overlay *ov) {
    if (ov->cfg->fallback_input != FALLBACK_AMX_MOUSE) return;
    ov->cfg->fallback_input = FALLBACK_JOYSTICK;
    if (ov->cpc) {
        ov->cpc->amx_mouse = false;
        amx_reset(&ov->cpc->amx, &ov->cpc->kbd);
    }
}

/* Turn off both MX4 pointer mice (SymbIface PS/2 + Albireo HID). Used when the
 * AMX fallback mouse is selected — mirrors the SymbIface<->Albireo exclusion.
 * These are MX4 hardware, so disabling them requires a cold boot. */
static void amx_take_pointer(Overlay *ov) {
    bool changed = false;
    if (ov->cfg->symbiface_mouse) {
        ov->cfg->symbiface_mouse = false;
        if (ov->cpc) ov->cpc->symbiface_mouse = false;
        changed = true;
    }
    if (ov->cfg->albireo_mouse) {
        ov->cfg->albireo_mouse = false;
        if (ov->cpc) {
            ov->cpc->albireo_mouse = false;
            ov->cpc->ch376.has_mouse = false;
            ch376_close(&ov->cpc->ch376);
            ch376_close(&ov->cpc->ch376_b);
            if (ov->cfg->albireo_image[0])
                ch376_open(&ov->cpc->ch376, ov->cfg->albireo_image);
        }
        changed = true;
    }
    if (changed) ov->needs_cold_boot = true;
}

static void activate_item(Overlay *ov, SDL_Keymod mods) {
    switch (ov->section) {

    case OV_GENERAL: {
        bool has_external_tape_row = (ov->cfg->model != MODEL_464);
        int logical = ov->row;
        if (!has_external_tape_row && ov->row >= 4) logical = ov->row + 1;
        switch (logical) {
        case 0: {
            CpcModel next;
            switch (ov->cfg->model) {
                case MODEL_464:  next = MODEL_664;  break;
                case MODEL_664:  next = MODEL_6128; break;
                default:         next = MODEL_464;  break;
            }
            config_set_model(ov->cfg, next);
            if (next == MODEL_6128 && ov->cfg->memory_kb < 128)
                ov->cfg->memory_kb = 128;
            else if (next != MODEL_6128 && ov->cfg->memory_kb != 64)
                ov->cfg->memory_kb = 64;
            ov->dirty = true;
            break;
        }
        case 1: {
            /* Memory — cycle through valid sizes for the current model. */
            static const int sizes[] = { 64, 128, 256, 512, 576, 768, 1024 };
            int n = (int)(sizeof(sizes) / sizeof(sizes[0]));
            int min_idx = (ov->cfg->model == MODEL_6128) ? 1 : 0;
            int cur = min_idx;
            for (int i = min_idx; i < n; i++)
                if (sizes[i] == ov->cfg->memory_kb) { cur = i; break; }
            ov->cfg->memory_kb = sizes[min_idx + (cur - min_idx + 1) % (n - min_idx)];
            ov->dirty = true;
            break;
        }
        case 2:
            ov->cfg->mx4 = !ov->cfg->mx4;
            if (ov->cpc) {
                ov->cpc->mx4 = ov->cfg->mx4;
                printer_set_connected(&ov->cpc->printer, ov->cfg->mx4);
            }
            leds_set_enabled(LED_PRINTER, ov->cfg->mx4);
            ov->dirty = true;
            break;
        case 3:
            ov->cfg->rom_board = !ov->cfg->rom_board;
            ov->dirty = true;
            break;
        case 4:
            /* External Tape — only reachable from this menu on the 6128. */
            ov->cfg->external_tape = !ov->cfg->external_tape;
            ov->dirty = true;
            break;
        case 5: {
            ov->dialog_kind  = DIALOG_LOWER_ROM;
            ov->dialog_ready = false;
            static const SDL_DialogFileFilter rom_filters[] = {
                { "ROM images", "rom;ROM" },
                { "All files",  "*"       },
            };
            SDL_ShowOpenFileDialog(overlay_file_callback, ov,
                ov->cpc ? ov->cpc->display.window : NULL,
                rom_filters, 2, ov->cfg->last_dir[0] ? ov->cfg->last_dir : NULL, false);
            break;
        }
        case 6: {
            ov->dialog_kind  = DIALOG_BASIC_ROM;
            ov->dialog_ready = false;
            static const SDL_DialogFileFilter rom_filters[] = {
                { "ROM images", "rom;ROM" },
                { "All files",  "*"       },
            };
            SDL_ShowOpenFileDialog(overlay_file_callback, ov,
                ov->cpc ? ov->cpc->display.window : NULL,
                rom_filters, 2, ov->cfg->last_dir[0] ? ov->cfg->last_dir : NULL, false);
            break;
        }
        case 7:
            ov->cfg->tinker = !ov->cfg->tinker;
            /* Don't leave the cursor parked on a tab that just disappeared. */
            if (!ov->cfg->tinker && ov->section == OV_TINKER) {
                ov->section = OV_GENERAL;
                ov->row = 0;
            }
            ov->dirty = true;
            break;
        case 8:
            /* Fallback Input: Joystick <-> AMX Mouse. Pure input routing —
             * mirror the flag live, no cold boot. */
            ov->cfg->fallback_input =
                (ov->cfg->fallback_input == FALLBACK_JOYSTICK)
                    ? FALLBACK_AMX_MOUSE : FALLBACK_JOYSTICK;
            if (ov->cfg->fallback_input == FALLBACK_AMX_MOUSE)
                amx_take_pointer(ov);   /* AMX and the MX4 mice can't coexist */
            if (ov->cpc) {
                ov->cpc->amx_mouse =
                    (ov->cfg->fallback_input == FALLBACK_AMX_MOUSE);
                /* Hand off row 9: drop any lingering joystick/AMX bits. */
                for (int col = AMX_UP; col <= AMX_FIRE2; col++)
                    kbd_key_up(&ov->cpc->kbd, AMX_ROW, col);
                amx_reset(&ov->cpc->amx, &ov->cpc->kbd);
            }
            ov->dirty = true;
            break;
        }
        break;
    }

    case OV_STORAGE:
        if ((ov->row == 0 || ov->row == 1) && floppy_accessible(ov)) {
            if (mods & SDL_KMOD_SHIFT)
                open_internal_disk_browser(ov, ov->row);
            else
                open_disk_dialog(ov, ov->row);
        } else if (ov->row == 2) {
            /* Tape — stub: file picker captures the .cdt path into
             * cfg.tape, but nothing reads it yet. */
            ov->dialog_kind  = DIALOG_TAPE;
            ov->dialog_ready = false;
            static const SDL_DialogFileFilter tape_filters[] = {
                { "CDT tape images", "cdt;CDT" },
                { "All files",       "*"       },
            };
            SDL_ShowOpenFileDialog(overlay_file_callback, ov,
                ov->cpc ? ov->cpc->display.window : NULL,
                tape_filters, 2, ov->cfg->last_dir[0] ? ov->cfg->last_dir : NULL, false);
        }
        break;

    case OV_ADVANCED:
        /* Without the MX4 bus connected, these cards are unplugged — block
         * toggle on those rows. Net4CPC / RTC / DD1 / Diag Cart / ROM Slots
         * have their own gating; the rest are nailed down here. */
        if (!ov->cfg->mx4 &&
            (ov->row == 0 || ov->row == 7 || ov->row == 8 ||
             ov->row == 9 || ov->row == 10 || ov->row == 11))
            break;
        switch (ov->row) {
        case 0:
            if (!ov->cfg->m4) {
                /* The only genuine conflict is Albireo: both decode port
                 * 0xFExx and M4ROM clashes with Albireo's UNIDOS tooling, so
                 * enabling M4 disables it. RTC (0xFD14), the SymbIface mouse
                 * (0xFD10), IDE and Net4CPC live on other ports and coexist
                 * with M4 — leave them untouched. config_apply_boards (run by
                 * overlay_check_board_changes) clears Albireo's now-inactive
                 * ROM slot, so no blanket slot wipe is needed here. */
                if (ov->cfg->albireo) {
                    ov->cfg->albireo = false;
                    ov->cfg->albireo_image[0] = '\0';
                    if (ov->cpc) {
                        ov->cpc->albireo = false;
                        ch376_close(&ov->cpc->ch376);
                    }
                }
                /* Reuse cached image if present (see cyboard/albireo). */
                if (ov->cfg->board_m4_image[0]) {
                    snprintf(ov->cfg->m4_image, sizeof(ov->cfg->m4_image),
                             "%s", ov->cfg->board_m4_image);
                    ov->cfg->m4 = true;
                    if (ov->cpc) {
                        ov->cpc->m4 = true;
                        m4_set_image(&ov->cpc->m4_card, ov->cfg->m4_image);
                    }
                    ov->needs_cold_boot = true;
                    ov->dirty = true;
                } else {
                    /* Enable: open file picker to select SD card image (raw FAT) */
                    ov->dialog_kind  = DIALOG_M4_IMAGE;
                    ov->dialog_ready = false;
                    static const SDL_DialogFileFilter m4_filters[] = {
                        { "SD card images", "img;IMG;bin;BIN;raw;RAW" },
                        { "All files",      "*"                        },
                    };
                    SDL_ShowOpenFileDialog(overlay_file_callback, ov,
                        ov->cpc ? ov->cpc->display.window : NULL,
                        m4_filters, 2, ov->cfg->last_dir[0] ? ov->cfg->last_dir : NULL, false);
                }
            } else {
                /* Disable: clear flag and unload M4ROM from its slot.
                 * Keep board_m4_image so the next enable doesn't
                 * re-prompt; clear it via Del on this row. */
                ov->cfg->m4 = false;
                ov->cfg->m4_image[0] = '\0';
                if (ov->cpc) {
                    ov->cpc->m4 = false;
                    m4_set_image(&ov->cpc->m4_card, "");
                }
                ov->cfg->rom_ext[M4_ROM_SLOT][0] = '\0';
                if (ov->cpc)
                    mem_unload_rom_ext(&ov->cpc->mem, M4_ROM_SLOT);
                ov->needs_cold_boot = true;
                ov->dirty = true;
            }
            break;
        case 1:
            ov->cfg->usifac = !ov->cfg->usifac;
            /* PerryFi rides on top of USIfAC's serial port — when the
             * port goes away the modem has nothing to plug into, so
             * force it off too. The re-init below picks up the new
             * combined state. */
            if (!ov->cfg->usifac) ov->cfg->perryfi = false;
            if (ov->cpc) {
                usifac_shutdown(&ov->cpc->usifac);
                usifac_init(&ov->cpc->usifac, ov->cfg->mx4 && ov->cfg->usifac,
                            ov->cfg->usifac_backend, ov->cfg->usifac_tcp_port,
                            ov->cfg->usifac_pty_link);
                perryfi_shutdown(&ov->cpc->perryfi);
                perryfi_init(&ov->cpc->perryfi,
                             ov->cfg->mx4 && ov->cfg->usifac && ov->cfg->perryfi);
                leds_set_enabled(LED_USIFAC,
                                 ov->cpc->mx4 && ov->cfg->usifac);
            }
            ov->dirty = true;
            break;
        case 2:
            ov->cfg->net4cpc = !ov->cfg->net4cpc;
            ov->dirty = true;
            break;
        case 3:
            /* RTC (port 0xFD14) is independent — it shares no ports with M4
             * and carries no ROM, so it coexists with every other card. */
            ov->cfg->rtc = !ov->cfg->rtc;
            if (ov->cpc) ov->cpc->rtc = ov->cfg->rtc;
            ov->dirty = true;
            break;
        case 4:
            if (ov->cfg->model == MODEL_464) {
                config_apply_dd1(ov->cfg, !ov->cfg->dd1);
                if (ov->cpc) {
                    if (ov->cfg->dd1)
                        mem_load_amsdos(&ov->cpc->mem, ov->cfg->rom_amsdos);
                    else
                        mem_unload_amsdos(&ov->cpc->mem);
                }
                ov->dirty = true;
            }
            break;
        case 5:
            if (ov->cfg->rom_board)
                ov->state = OV_STATE_ROMSLOTS;
            break;
        case 6: {
            char diag[CONFIG_PATH_MAX];
            config_default_diag(diag, sizeof(diag));
            if (access(diag, R_OK) != 0) break;  /* greyed out — ROM missing */
            bool active = strcmp(ov->cfg->rom_os, diag) == 0;
            if (active) {
                config_default_os(ov->cfg->model,
                    ov->cfg->rom_os, sizeof(ov->cfg->rom_os));
            } else {
                snprintf(ov->cfg->rom_os, CONFIG_PATH_MAX, "%s", diag);
            }
            ov->dirty = true;
            break;
        }
        case 7:
            if (!ov->cfg->symbiface_ide) {
                /* Enabling: if the cyboard board template has a cached
                 * IDE image, reuse it (no dialog) — config_apply_boards
                 * runs after this case and copies the cached image
                 * into cfg.ide_image. Otherwise open the file picker. */
                if (ov->cfg->board_cyboard_image[0]) {
                    ov->cfg->symbiface_ide = true;
                    ov->dirty = true;
                } else {
                    ov->dialog_kind  = DIALOG_IDE;
                    ov->dialog_ready = false;
                    static const SDL_DialogFileFilter ide_filters[] = {
                        { "Disk images", "img;IMG;hdf;HDF;raw;RAW" },
                        { "All files",   "*"                       },
                    };
                    SDL_ShowOpenFileDialog(overlay_file_callback, ov,
                        ov->cpc ? ov->cpc->display.window : NULL,
                        ide_filters, 2, ov->cfg->last_dir[0] ? ov->cfg->last_dir : NULL, false);
                }
            } else {
                /* Disabling: clear the LIVE image but keep the cached
                 * board_cyboard_image so the next enable doesn't
                 * re-prompt. The user clears the cache via Del on
                 * this row (see key handler for OV_ADVANCED). */
                ov->cfg->symbiface_ide = false;
                ov->cfg->ide_image[0]  = '\0';
                ov->dirty = true;
            }
            break;
        case 8:
            ov->cfg->symbiface_mouse = !ov->cfg->symbiface_mouse;
            /* SymbIface Mouse, Albireo Mouse and the AMX fallback mouse all
             * drive the host pointer — only one can own it at a time. */
            if (ov->cfg->symbiface_mouse) amx_release_pointer(ov);
            if (ov->cfg->symbiface_mouse && ov->cfg->albireo_mouse) {
                ov->cfg->albireo_mouse = false;
                if (ov->cpc) {
                    ov->cpc->albireo_mouse = false;
                    ov->cpc->ch376.has_mouse = false;
                    ch376_close(&ov->cpc->ch376);
                    ch376_close(&ov->cpc->ch376_b);
                    if (ov->cfg->albireo_image[0])
                        ch376_open(&ov->cpc->ch376, ov->cfg->albireo_image);
                }
                ov->needs_cold_boot = true;
            }
            ov->dirty = true;
            break;
        case 9:
            /* Albireo Mouse toggle (opt-in HID polling on chip A plus
             * storage on chip B at 0xFE40). Requires the dual-CH376
             * card to be enabled; mutually exclusive with the SymbIface
             * PS/2 mouse. */
            if (!ov->cfg->mx4 || !ov->cfg->albireo) break;
            ov->cfg->albireo_mouse = !ov->cfg->albireo_mouse;
            if (ov->cfg->albireo_mouse) amx_release_pointer(ov);
            if (ov->cfg->albireo_mouse && ov->cfg->symbiface_mouse) {
                ov->cfg->symbiface_mouse = false;
                if (ov->cpc) ov->cpc->symbiface_mouse = false;
            }
            if (ov->cpc) {
                ov->cpc->albireo_mouse = ov->cfg->albireo_mouse;
                ov->cpc->ch376.has_mouse = ov->cfg->albireo_mouse;
                ov->cpc->ch376_b.has_mouse = false;
                ch376_close(&ov->cpc->ch376);
                ch376_close(&ov->cpc->ch376_b);
                if (ov->cfg->albireo_image[0]) {
                    CH376 *storage = ov->cfg->albireo_mouse
                                   ? &ov->cpc->ch376_b : &ov->cpc->ch376;
                    ch376_open(storage, ov->cfg->albireo_image);
                }
            }
            ov->needs_cold_boot = true;
            ov->dirty = true;
            break;
        case 10:
            if (!ov->cfg->albireo) {
                /* Mutually exclusive with M4 (both claim port 0xFExx). */
                if (ov->cfg->m4) {
                    ov->cfg->m4 = false;
                    ov->cfg->m4_image[0] = '\0';
                    ov->cfg->rom_ext[M4_ROM_SLOT][0] = '\0';
                    if (ov->cpc) {
                        ov->cpc->m4 = false;
                        m4_set_image(&ov->cpc->m4_card, "");
                        mem_unload_rom_ext(&ov->cpc->mem, M4_ROM_SLOT);
                    }
                    ov->needs_cold_boot = true;
                }
                /* Reuse cached image if present (see case 7). */
                if (ov->cfg->board_albireo_image[0]) {
                    ov->cfg->albireo = true;
                    ov->dirty = true;
                } else {
                    ov->dialog_kind  = DIALOG_ALBIREO;
                    ov->dialog_ready = false;
                    static const SDL_DialogFileFilter alb_filters[] = {
                        { "USB drive images", "img;IMG;bin;BIN;raw;RAW;iso;ISO" },
                        { "All files",        "*"                                },
                    };
                    SDL_ShowOpenFileDialog(overlay_file_callback, ov,
                        ov->cpc ? ov->cpc->display.window : NULL,
                        alb_filters, 2, ov->cfg->last_dir[0] ? ov->cfg->last_dir : NULL, false);
                }
            } else {
                /* Disabling the card also drops the Albireo Mouse — it
                 * has no chip to talk to. Keep board_albireo_image for
                 * the next enable cycle. */
                ov->cfg->albireo = false;
                ov->cfg->albireo_image[0] = '\0';
                ov->cfg->albireo_mouse = false;
                if (ov->cpc) {
                    ov->cpc->albireo = false;
                    ov->cpc->albireo_mouse = false;
                    ov->cpc->ch376.has_mouse = false;
                    ch376_close(&ov->cpc->ch376);
                    ch376_close(&ov->cpc->ch376_b);
                }
                ov->dirty = true;
            }
            break;
        case 11: {
            bool all = ov->cfg->net4cpc && ov->cfg->rtc &&
                       ov->cfg->symbiface_ide && ov->cfg->symbiface_mouse;
            bool enable = !all;
            /* The Cyboard pack (Net4CPC, RTC, IDE, mouse) shares no ports
             * with M4 and coexists with it — no M4 teardown here. Only
             * Albireo genuinely conflicts, and Cyboard doesn't include it. */
            ov->cfg->net4cpc         = enable;
            ov->cfg->rtc             = enable;
            ov->cfg->symbiface_ide   = enable;
            ov->cfg->symbiface_mouse = enable;
            if (!enable) {
                ov->cfg->ide_image[0] = '\0';
                ov->dirty = true;
            } else if (!ov->cfg->ide_image[0]) {
                /* Enabling with no image selected — use the cached
                 * board_cyboard_image if set; else open a file picker
                 * (config_apply_boards runs in handle_event after this
                 * and will sync the cache → live too, but doing it
                 * here keeps the case self-contained). */
                if (ov->cfg->board_cyboard_image[0]) {
                    snprintf(ov->cfg->ide_image, sizeof(ov->cfg->ide_image),
                             "%s", ov->cfg->board_cyboard_image);
                    ov->dirty = true;
                } else {
                    ov->dialog_kind  = DIALOG_IDE;
                    ov->dialog_ready = false;
                    static const SDL_DialogFileFilter ide_filters[] = {
                        { "Disk images", "img;IMG;hdf;HDF;raw;RAW" },
                        { "All files",   "*"                       },
                    };
                    SDL_ShowOpenFileDialog(overlay_file_callback, ov,
                        ov->cpc ? ov->cpc->display.window : NULL,
                        ide_filters, 2, ov->cfg->last_dir[0] ? ov->cfg->last_dir : NULL, false);
                    /* dirty set by file callback once image is chosen */
                }
            } else {
                ov->dirty = true;
            }
            break;
        }
        case 12:
            /* PDF printer: when turning ON, pop a folder picker so the
             * user can name the output directory. Picker callback path
             * (DIALOG_PRINTER_DIR in overlay_tick) finishes the enable
             * by setting pdf_printer + pdf_printer_dir and arming the
             * printer module. Turning OFF is immediate. */
            if (!ov->cfg->mx4) break;
            if (ov->cfg->pdf_printer) {
                ov->cfg->pdf_printer = false;
                if (ov->cpc)
                    printer_set_pdf_enabled(&ov->cpc->printer, false);
                ov->dirty = true;
            } else {
                ov->dialog_kind  = DIALOG_PRINTER_DIR;
                ov->dialog_ready = false;
                SDL_ShowOpenFolderDialog(overlay_file_callback, ov,
                    ov->cpc ? ov->cpc->display.window : NULL,
                    ov->cfg->pdf_printer_dir[0] ? ov->cfg->pdf_printer_dir
                        : (ov->cfg->last_dir[0] ? ov->cfg->last_dir : NULL),
                    false);
            }
            break;
        case 13:
            /* Wi-Fi modem (PerryFi): needs the USIfAC serial port to
             * plug into. Toggle re-inits the modem with the new enable
             * state; the USIfAC data path checks perryfi.present per
             * byte so the switch takes effect immediately. */
            if (!ov->cfg->mx4 || !ov->cfg->usifac) break;
            ov->cfg->perryfi = !ov->cfg->perryfi;
            if (ov->cpc) {
                perryfi_shutdown(&ov->cpc->perryfi);
                perryfi_init(&ov->cpc->perryfi, ov->cfg->perryfi);
            }
            ov->dirty = true;
            break;
        }
        break;

    case OV_TINKER:
        switch (tinker_logical_row(ov, ov->row)) {
        case -6:
            ov->cfg->crt_blue = cycle_crt_percent(ov->cfg->crt_blue, 50, 150);
            overlay_apply_crt(ov);
            ov->dirty = true;
            break;
        case -5:
            ov->cfg->crt_green = cycle_crt_percent(ov->cfg->crt_green, 50, 150);
            overlay_apply_crt(ov);
            ov->dirty = true;
            break;
        case -4:
            ov->cfg->crt_red = cycle_crt_percent(ov->cfg->crt_red, 50, 150);
            overlay_apply_crt(ov);
            ov->dirty = true;
            break;
        case -3:
            ov->cfg->crt_contrast = cycle_crt_percent(ov->cfg->crt_contrast,
                                                      50, 150);
            overlay_apply_crt(ov);
            ov->dirty = true;
            break;
        case -2:
            ov->cfg->crt_brightness -= 5;
            if (ov->cfg->crt_brightness < 50)
                ov->cfg->crt_brightness = 100;
            overlay_apply_crt(ov);
            ov->dirty = true;
            break;
        case -1:
            ov->cfg->crt_scanlines = cycle_crt_percent(ov->cfg->crt_scanlines,
                                                       0, 95);
            overlay_apply_crt(ov);
            ov->dirty = true;
            break;
        case 0:
            ov->cfg->fullscreen_smoothing = !ov->cfg->fullscreen_smoothing;
            if (ov->cpc)
                display_set_smoothing(&ov->cpc->display,
                                      ov->cfg->fullscreen_smoothing);
            ov->dirty = true;
            break;
        case 1:
            ov->cfg->real_crt = !ov->cfg->real_crt;
            overlay_apply_crt(ov);
            ov->dirty = true;
            break;
        case 2: {
            /* Load Snapshot — open file picker for .sna */
            ov->dialog_kind  = DIALOG_SNAPSHOT_LOAD;
            ov->dialog_ready = false;
            static const SDL_DialogFileFilter sna_filters[] = {
                { "SNA snapshots", "sna;SNA" },
                { "All files",     "*"       },
            };
            SDL_ShowOpenFileDialog(overlay_file_callback, ov,
                ov->cpc ? ov->cpc->display.window : NULL,
                sna_filters, 2, ov->cfg->last_dir[0] ? ov->cfg->last_dir : NULL, false);
            break;
        }
        case 3: {
            /* Save Snapshot — open save dialog */
            ov->dialog_kind  = DIALOG_SNAPSHOT_SAVE;
            ov->dialog_ready = false;
            static const SDL_DialogFileFilter sna_filters[] = {
                { "SNA snapshots", "sna;SNA" },
                { "All files",     "*"       },
            };
            SDL_ShowSaveFileDialog(overlay_file_callback, ov,
                ov->cpc ? ov->cpc->display.window : NULL,
                sna_filters, 2, ov->cfg->last_dir[0] ? ov->cfg->last_dir : NULL);
            break;
        }
        case 4:
            if (TAP_SUPPORTED && ov->cfg->net4cpc) {
                ov->cfg->net4cpc_tap = !ov->cfg->net4cpc_tap;
                ov->dirty = true;
                ov->needs_cold_boot = true;
            }
            break;
        case 6:
            ov->cfg->debug = !ov->cfg->debug;
            g_debug_enabled = ov->cfg->debug ? 1 : 0;
            ov->dirty = true;
            /* No cold boot needed — debug machinery is checked at every
             * site, so the change takes effect on the next instruction. */
            break;
        case 7:
            if (!WEBMCAP_SUPPORTED) break;
            if (videocap_active()) {
                videocap_stop();
            } else {
                ov->dialog_kind  = DIALOG_VIDEO_CAPTURE;
                ov->dialog_ready = false;
                static const SDL_DialogFileFilter webm_filters[] = {
                    { "WebM (VP9)", "webm;WEBM" },
                    { "All files",  "*"         },
                };
                SDL_ShowSaveFileDialog(overlay_file_callback, ov,
                    ov->cpc ? ov->cpc->display.window : NULL,
                    webm_filters, 2, ov->cfg->last_dir[0] ? ov->cfg->last_dir : NULL);
            }
            break;
        case 8:
            ov->cfg->gif_width = cycle_gif_width(ov->cfg->gif_width);
            ov->dirty = true;
            break;
        case 9:
            ov->cfg->gif_fps = cycle_gif_fps(ov->cfg->gif_fps);
            ov->dirty = true;
            break;
        case 10:
            if (!FFMPEG_GIF_SUPPORTED) break;
            ov->cfg->gif_ffmpeg = !ov->cfg->gif_ffmpeg;
            ov->dirty = true;
            break;
        case 11:
            if (!ov->cfg->usifac) break;
            /* Flip pty ↔ tcp; re-init the device on the new backend. */
            if (!strcmp(ov->cfg->usifac_backend, "tcp"))
                snprintf(ov->cfg->usifac_backend,
                         sizeof(ov->cfg->usifac_backend), "pty");
            else
                snprintf(ov->cfg->usifac_backend,
                         sizeof(ov->cfg->usifac_backend), "tcp");
            if (ov->cpc) {
                usifac_shutdown(&ov->cpc->usifac);
                usifac_init(&ov->cpc->usifac, ov->cfg->mx4 && ov->cfg->usifac,
                            ov->cfg->usifac_backend, ov->cfg->usifac_tcp_port,
                            ov->cfg->usifac_pty_link);
            }
            ov->dirty = true;
            break;
        case 12:
            /* USIfAC PTY link: Enter starts an inline text editor seeded
             * with the current value (empty if none). Commit applies the
             * new path and re-inits USIfAC; clearing the field and
             * committing removes the alias. */
            if (!ov->cfg->usifac) break;
            if (strcmp(ov->cfg->usifac_backend, "pty")) break;
            ov->pty_link_editing = true;
            snprintf(ov->pty_link_edit_buf, sizeof(ov->pty_link_edit_buf),
                     "%s", ov->cfg->usifac_pty_link);
            if (ov->cpc && ov->cpc->display.window)
                SDL_StartTextInput(ov->cpc->display.window);
            break;
        case 13: {
            MonoMode next;
            switch (ov->cfg->monochrome) {
                case MONO_OFF:   next = MONO_GREEN; break;
                case MONO_GREEN: next = MONO_AMBER; break;
                case MONO_AMBER: next = MONO_WHITE; break;
                default:         next = MONO_OFF;   break;
            }
            ov->cfg->monochrome = next;
            if (ov->cpc) ga_set_monochrome(&ov->cpc->ga, next);
            ov->dirty = true;
            break;
        }
        case 14:
            if (!ov->cfg->mx4) break;
            ov->cfg->print_sink = (ov->cfg->print_sink == PRINTER_SINK_PDF)
                                  ? PRINTER_SINK_REAL_PRINTER : PRINTER_SINK_PDF;
            if (ov->cpc)
                printer_set_sink(&ov->cpc->printer,
                                 ov->cfg->print_sink == PRINTER_SINK_REAL_PRINTER
                                     ? PRINT_SINK_REAL : PRINT_SINK_PDF);
            ov->dirty = true;
            break;
        case 15:
            /* Volume: step 5% per Enter, wrap 0→100. Use Left/Right for
             * finer control (handled in handle_event). */
            ov->cfg->audio_volume += 5;
            if (ov->cfg->audio_volume > 100) ov->cfg->audio_volume = 0;
            if (ov->cpc) psg_set_volume(&ov->cpc->psg, ov->cfg->audio_volume);
            ov->dirty = true;
            break;
        case 16:
            /* Stereo separation: cycle mono → half → full. */
            ov->cfg->audio_stereo_sep =
                ov->cfg->audio_stereo_sep == 0     ? 128 :
                ov->cfg->audio_stereo_sep == 128   ? 255 : 0;
            if (ov->cpc) psg_set_stereo(&ov->cpc->psg, ov->cfg->audio_stereo_sep);
            ov->dirty = true;
            break;
        case 17:
            /* Notifications: cycle screen -> console -> off. */
            ov->cfg->notifications =
                (NotifyMode)((ov->cfg->notifications + 1) % 3);
            notify_set_mode(ov->cfg->notifications);
            ov->dirty = true;
            break;
        case 18:
            /* Web GUI: live start/stop, no reboot. On start failure the
             * flag stays off (webgui_start posted the toast). */
            if (webgui_active()) {
                webgui_stop();
                ov->cfg->web_gui = false;
            } else if (webgui_start(ov->cfg->web_port)) {
                ov->cfg->web_gui = true;
            }
            ov->dirty = true;
            break;
        }
        break;

    default:
        break;
    }
}

static void try_close(Overlay *ov) {
    browser_clear_entries(ov);
    if (ov->dirty)
        ov->state = OV_STATE_CONFIRM;
    else
        ov->visible = false;
}

/* ---- Public API ---- */

/* File-dialog callback — called from SDL's thread on some platforms.
 * On cancel we clear ALL dialog state, otherwise a subsequent dialog
 * inherits a stale dialog_kind/dialog_slot and overlay_tick dispatches
 * the wrong action against an unrelated slot index. The release barrier
 * pairs with the acquire in overlay_tick so dialog_path is fully
 * published before the consumer sees dialog_ready=true. */
static void overlay_file_callback(void *userdata, const char * const *files,
                                  int filter) {
    (void)filter;
    Overlay *ov = userdata;
    if (!files) {
        snprintf(ov->dialog_error, sizeof(ov->dialog_error), "%s",
                 SDL_GetError());
        SDL_MemoryBarrierRelease();
        ov->dialog_failed = true;
    } else if (files[0]) {
        snprintf(ov->dialog_path, sizeof(ov->dialog_path), "%s", files[0]);
        SDL_MemoryBarrierRelease();
        ov->dialog_ready = true;
    } else {
        ov->dialog_drive = -1;
        ov->dialog_slot  = -1;
        ov->dialog_kind  = DIALOG_NONE;
    }
}

void overlay_init(Overlay *ov, Config *cfg, CPC *cpc, bool sdl_fm) {
    memset(ov, 0, sizeof(*ov));
    ov->cfg          = cfg;
    ov->cpc          = cpc;
    ov->sdl_fm       = sdl_fm;
    ov->dialog_kind  = DIALOG_NONE;
    ov->dialog_drive = -1;
    ov->dialog_slot  = -1;
    ov->last_m4            = cfg->m4;
    ov->last_albireo       = cfg->albireo;
    ov->last_symbiface_ide = cfg->symbiface_ide;
}

void overlay_quit(Overlay *ov) {
    browser_clear_entries(ov);
}

/* Check whether any of the ROM-owning hardware toggles flipped since
 * we last looked; if so, re-apply per-board ROM templates and request
 * a cold boot so the new board's grouping loads from scratch. Called
 * from both overlay_handle_event (after activate_item) and
 * overlay_tick (after dialog-callback completion), so changes via
 * either path are caught. */
static void overlay_check_board_changes(Overlay *ov) {
    bool changed =
        (ov->cfg->m4            != ov->last_m4) ||
        (ov->cfg->albireo       != ov->last_albireo) ||
        (ov->cfg->symbiface_ide != ov->last_symbiface_ide);
    if (!changed) return;
    config_apply_boards(ov->cfg);
    ov->dirty = true;
    ov->needs_cold_boot = true;
    ov->last_m4            = ov->cfg->m4;
    ov->last_albireo       = ov->cfg->albireo;
    ov->last_symbiface_ide = ov->cfg->symbiface_ide;
}

/* Stash the parent directory of the freshly picked path in cfg->last_dir
 * so the next file/folder dialog starts where this one left off (#107).
 * Folder pickers land on the *parent* of the chosen folder which is the
 * SDL default for "where the dialog opens" — slightly suboptimal but
 * uniform with file pickers and avoids special-casing per dialog kind. */
static void remember_last_dir(Config *cfg, const char *picked) {
    if (!picked || !picked[0]) return;
    const char *sep = strrchr(picked, '/');
#ifdef _WIN32
    const char *bs = strrchr(picked, '\\');
    if (bs && (!sep || bs > sep)) sep = bs;
#endif
    if (!sep || sep == picked) return;
    size_t n = (size_t)(sep - picked);
    if (n >= sizeof(cfg->last_dir)) n = sizeof(cfg->last_dir) - 1;
    memcpy(cfg->last_dir, picked, n);
    cfg->last_dir[n] = '\0';
}

void overlay_tick(Overlay *ov) {
    if (ov->dialog_failed) {
        SDL_MemoryBarrierAcquire();
        DialogKind failed_kind = ov->dialog_kind;
        int failed_drive = ov->dialog_drive;
        ov->dialog_failed = false;
        ov->dialog_kind = DIALOG_NONE;
        ov->dialog_drive = -1;
        ov->dialog_slot = -1;

        if (failed_kind == DIALOG_DISK && ov->visible
            && failed_drive >= 0 && failed_drive < 2) {
            open_internal_disk_browser(ov, failed_drive);
        } else {
            notify_post("File picker unavailable: %s",
                        ov->dialog_error[0]
                            ? ov->dialog_error : "unknown SDL error");
        }
        return;
    }
    if (!ov->dialog_ready) return;
    SDL_MemoryBarrierAcquire();
    ov->dialog_ready = false;
    remember_last_dir(ov->cfg, ov->dialog_path);

    if (ov->dialog_kind == DIALOG_DISK && ov->dialog_drive >= 0 && ov->dialog_drive < 2) {
        int drv = ov->dialog_drive;
        ov->dialog_drive = -1;
        char *dest = (drv == 0) ? ov->cfg->disk_a : ov->cfg->disk_b;
        snprintf(dest, CONFIG_PATH_MAX, "%s", ov->dialog_path);
        if (ov->cpc) {
            Disk *d = &ov->cpc->drive[drv];
            disk_eject(d);
            if (dest[0] && disk_load(d, dest) < 0) {
                fprintf(stderr, "1984: failed to load %s\n", dest);
                dest[0] = '\0';
            }
        }
        ov->dirty = true;
    } else if (ov->dialog_kind == DIALOG_DISK_NEW &&
               ov->dialog_drive >= 0 && ov->dialog_drive < 2) {
        int drv = ov->dialog_drive;
        ov->dialog_drive = -1;
        char *dest = (drv == 0) ? ov->cfg->disk_a : ov->cfg->disk_b;
        if (disk_create_blank(ov->dialog_path) == 0) {
            snprintf(dest, CONFIG_PATH_MAX, "%s", ov->dialog_path);
            if (ov->cpc) {
                Disk *d = &ov->cpc->drive[drv];
                disk_eject(d);
                if (disk_load(d, dest) < 0) {
                    fprintf(stderr, "1984: created %s but failed to mount\n", dest);
                    dest[0] = '\0';
                }
            }
            ov->dirty = true;
        }
    } else if (ov->dialog_kind == DIALOG_TAPE) {
        /* Stub — just record the path. PSG cassette input not wired yet. */
        snprintf(ov->cfg->tape, CONFIG_PATH_MAX, "%s", ov->dialog_path);
        ov->dirty = true;
    } else if (ov->dialog_kind == DIALOG_PRINTER_DIR) {
        snprintf(ov->cfg->pdf_printer_dir, sizeof(ov->cfg->pdf_printer_dir),
                 "%s", ov->dialog_path);
        ov->cfg->pdf_printer = true;
        if (ov->cpc) {
            printer_set_pdf_output_dir(&ov->cpc->printer, ov->cfg->pdf_printer_dir);
            printer_set_pdf_enabled(&ov->cpc->printer, true);
        }
        ov->dirty = true;
    } else if (ov->dialog_kind == DIALOG_LOWER_ROM) {
        snprintf(ov->cfg->rom_os, CONFIG_PATH_MAX, "%s", ov->dialog_path);
        if (ov->cpc)
            mem_load_os(&ov->cpc->mem, ov->dialog_path);
        ov->needs_cold_boot = true;
        ov->dirty = true;
    } else if (ov->dialog_kind == DIALOG_BASIC_ROM) {
        snprintf(ov->cfg->rom_basic, CONFIG_PATH_MAX, "%s", ov->dialog_path);
        /* No live reloader for BASIC; the cold-boot triggered on Save
         * by the rom_basic change reloads it via cpc_init/mem_load_rom. */
        ov->needs_cold_boot = true;
        ov->dirty = true;
    } else if (ov->dialog_kind == DIALOG_IDE) {
        snprintf(ov->cfg->ide_image, CONFIG_PATH_MAX, "%s", ov->dialog_path);
        /* Also stash in the cyboard board template so the next enable
         * cycle reuses this path without re-prompting. */
        snprintf(ov->cfg->board_cyboard_image,
                 sizeof(ov->cfg->board_cyboard_image),
                 "%s", ov->dialog_path);
        ov->cfg->symbiface_ide = true;
        ov->dirty = true;
    } else if (ov->dialog_kind == DIALOG_ALBIREO) {
        snprintf(ov->cfg->albireo_image, CONFIG_PATH_MAX, "%s", ov->dialog_path);
        snprintf(ov->cfg->board_albireo_image,
                 sizeof(ov->cfg->board_albireo_image),
                 "%s", ov->dialog_path);
        ov->cfg->albireo = true;
        if (ov->cpc) {
            ov->cpc->albireo = true;
            ov->cpc->albireo_mouse = ov->cfg->albireo_mouse;
            ch376_close(&ov->cpc->ch376);
            ch376_close(&ov->cpc->ch376_b);
            ov->cpc->ch376.has_mouse = ov->cfg->albireo_mouse;
            ov->cpc->ch376_b.has_mouse = false;
            CH376 *storage = ov->cfg->albireo_mouse
                           ? &ov->cpc->ch376_b : &ov->cpc->ch376;
            ch376_open(storage, ov->cfg->albireo_image);
        }
        ov->dirty = true;
    } else if (ov->dialog_kind == DIALOG_SNAPSHOT_LOAD) {
        if (ov->cpc && ov->dialog_path[0])
            snapshot_load(ov->cpc, ov->dialog_path);
    } else if (ov->dialog_kind == DIALOG_SNAPSHOT_SAVE) {
        if (ov->cpc && ov->dialog_path[0]) {
            /* Auto-append .sna if user didn't give an extension */
            char path[512];
            snprintf(path, sizeof(path), "%s", ov->dialog_path);
            char *dot = strrchr(path, '.');
            char *slash = strrchr(path, '/');
            if (!dot || (slash && dot < slash))
                strncat(path, ".sna", sizeof(path) - strlen(path) - 1);
            snapshot_save(ov->cpc, path);
        }
    } else if (ov->dialog_kind == DIALOG_VIDEO_CAPTURE) {
        if (ov->dialog_path[0]) {
            char path[512];
            snprintf(path, sizeof(path), "%s", ov->dialog_path);
            char *dot = strrchr(path, '.');
            char *slash = strrchr(path, '/');
            if (!dot || (slash && dot < slash))
                strncat(path, ".webm", sizeof(path) - strlen(path) - 1);
            videocap_start(path, ov->cfg->gif_width, ov->cfg->gif_fps,
                           ov->cfg->gif_ffmpeg);
        }
    } else if (ov->dialog_kind == DIALOG_M4_IMAGE) {
        /* User picked an SD card image — enable M4, load its ROM, and seed
         * the M4 board's config buffer from the ROM defaults.
         * If a sibling directory exists with the same base name (e.g. picking
         * SDCARD.img and SDCARD/ sits next to it), wire it up as the file-API
         * backing so BASIC's cat/load/save keep working. */
        snprintf(ov->cfg->m4_image, CONFIG_PATH_MAX, "%s", ov->dialog_path);
        snprintf(ov->cfg->board_m4_image, sizeof(ov->cfg->board_m4_image),
                 "%s", ov->dialog_path);
        ov->cfg->m4 = true;

        char sibling[CONFIG_PATH_MAX];
        snprintf(sibling, sizeof(sibling), "%s", ov->dialog_path);
        char *dot = strrchr(sibling, '.');
        if (dot) *dot = '\0';
        struct stat st;
        if (stat(sibling, &st) == 0 && S_ISDIR(st.st_mode))
            snprintf(ov->cfg->m4_path, CONFIG_PATH_MAX, "%s", sibling);
        else
            ov->cfg->m4_path[0] = '\0';

        char m4rom[CONFIG_PATH_MAX];
        config_default_m4rom(m4rom, sizeof(m4rom));
        snprintf(ov->cfg->rom_ext[M4_ROM_SLOT], CONFIG_PATH_MAX, "%s", m4rom);
        if (ov->cpc) {
            ov->cpc->m4 = true;
            m4_set_image(&ov->cpc->m4_card, ov->dialog_path);
            snprintf(ov->cpc->m4_card.root, M4_PATH_MAX, "%s", ov->cfg->m4_path);
            mem_load_rom_ext(&ov->cpc->mem, M4_ROM_SLOT, m4rom);
            memcpy(ov->cpc->m4_card.cfg_mem,
                   &ov->cpc->mem.rom_ext[M4_ROM_SLOT][0xF400 - 0xC000],
                   sizeof(ov->cpc->m4_card.cfg_mem));
        }
        ov->needs_cold_boot = true;
        ov->dirty = true;
    } else if (ov->dialog_kind == DIALOG_ROMSLOT && ov->dialog_slot >= 0) {
        int slot = ov->dialog_slot;
        ov->dialog_slot = -1;
        snprintf(ov->cfg->rom_ext[slot], CONFIG_PATH_MAX, "%s", ov->dialog_path);
        if (ov->cpc)
            mem_load_rom_ext(&ov->cpc->mem, slot, ov->dialog_path);
        ov->needs_cold_boot = true;
        ov->dirty = true;
    }
    ov->dialog_kind = DIALOG_NONE;
    /* Catch board toggles that flipped via a file-dialog callback
     * (DIALOG_IDE → cfg.symbiface_ide, DIALOG_ALBIREO → cfg.albireo,
     * DIALOG_M4_IMAGE → cfg.m4). Same effect as the call in
     * overlay_handle_event after activate_item. */
    overlay_check_board_changes(ov);
}

bool overlay_handle_event(Overlay *ov, SDL_Event *ev) {
    /* Inline editor for the USIfAC PTY-link path. Captures TEXT_INPUT
     * events for printable bytes and KEY_DOWN for control keys; eats
     * everything until Enter or Esc, so navigation keys can't escape
     * the field by accident. */
    if (ov->visible && ov->pty_link_editing) {
        if (ev->type == SDL_EVENT_TEXT_INPUT) {
            size_t len = strlen(ov->pty_link_edit_buf);
            const char *t = ev->text.text;
            while (*t && len + 1 < sizeof(ov->pty_link_edit_buf))
                ov->pty_link_edit_buf[len++] = *t++;
            ov->pty_link_edit_buf[len] = '\0';
            return true;
        }
        if (ev->type != SDL_EVENT_KEY_DOWN) return true;
        SDL_Keycode k = ev->key.key;
        if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
            snprintf(ov->cfg->usifac_pty_link,
                     sizeof(ov->cfg->usifac_pty_link), "%s",
                     ov->pty_link_edit_buf);
            if (ov->cpc) {
                usifac_shutdown(&ov->cpc->usifac);
                usifac_init(&ov->cpc->usifac, ov->cfg->mx4 && ov->cfg->usifac,
                            ov->cfg->usifac_backend, ov->cfg->usifac_tcp_port,
                            ov->cfg->usifac_pty_link);
            }
            ov->dirty = true;
            ov->pty_link_editing = false;
            if (ov->cpc && ov->cpc->display.window)
                SDL_StopTextInput(ov->cpc->display.window);
        } else if (k == SDLK_ESCAPE) {
            ov->pty_link_editing = false;
            if (ov->cpc && ov->cpc->display.window)
                SDL_StopTextInput(ov->cpc->display.window);
        } else if (k == SDLK_BACKSPACE) {
            size_t len = strlen(ov->pty_link_edit_buf);
            if (len > 0) ov->pty_link_edit_buf[len - 1] = '\0';
        } else if (k == SDLK_DELETE) {
            ov->pty_link_edit_buf[0] = '\0';
        }
        return true;
    }

    if (ev->type != SDL_EVENT_KEY_DOWN) return false;

    SDL_Scancode sc = ev->key.scancode;

    /* F9 always toggles */
    if (sc == SDL_SCANCODE_F9) {
        if (!ov->visible) {
            ov->visible = true;
            ov->state   = OV_STATE_MENU;
            ov->dirty   = false;
            ov->saved   = *ov->cfg;   /* snapshot */
        } else {
            try_close(ov);
        }
        return true;
    }

    if (!ov->visible) return false;

    if (ov->state == OV_STATE_FILE_BROWSER) {
        browser_handle_key(ov, ev->key.key);
        return true;
    }

    /* ---- Confirm dialog ---- */
    if (ov->state == OV_STATE_CONFIRM) {
        switch (sc) {
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: {
            config_save(ov->cfg);
            /* cold boot needed if model, memory, DD1, or any ROM slot changed */
            bool boot = (ov->cfg->model         != ov->saved.model)         ||
                        (ov->cfg->memory_kb     != ov->saved.memory_kb)     ||
                        (ov->cfg->mx4           != ov->saved.mx4)           ||
                        (ov->cfg->rom_board     != ov->saved.rom_board)     ||
                        (ov->cfg->dd1           != ov->saved.dd1)           ||
                        (ov->cfg->net4cpc       != ov->saved.net4cpc)       ||
                        (ov->cfg->net4cpc_tap   != ov->saved.net4cpc_tap)   ||
                        strcmp(ov->cfg->net4cpc_tap_host_ip,
                               ov->saved.net4cpc_tap_host_ip)               ||
                        strcmp(ov->cfg->net4cpc_tap_netmask,
                               ov->saved.net4cpc_tap_netmask)               ||
                        strcmp(ov->cfg->net4cpc_tap_lease_start,
                               ov->saved.net4cpc_tap_lease_start)           ||
                        strcmp(ov->cfg->net4cpc_tap_lease_end,
                               ov->saved.net4cpc_tap_lease_end)             ||
                        (ov->cfg->symbiface_ide != ov->saved.symbiface_ide) ||
                        (ov->cfg->symbiface_mouse != ov->saved.symbiface_mouse) ||
                        strcmp(ov->cfg->ide_image, ov->saved.ide_image)     ||
                        (ov->cfg->albireo       != ov->saved.albireo)       ||
                        strcmp(ov->cfg->albireo_image, ov->saved.albireo_image) ||
                        strcmp(ov->cfg->rom_os,    ov->saved.rom_os)     ||
                        strcmp(ov->cfg->rom_basic, ov->saved.rom_basic) ||
                        (ov->cfg->external_tape != ov->saved.external_tape) ||
                        strcmp(ov->cfg->tape, ov->saved.tape);
            if (!boot) {
                for (int i = 0; i < ROM_EXT_COUNT; i++) {
                    if (strcmp(ov->cfg->rom_ext[i], ov->saved.rom_ext[i])) {
                        boot = true; break;
                    }
                }
            }
            ov->needs_cold_boot = boot;
            ov->dirty   = false;
            ov->visible = false;
            break;
        }
        case SDL_SCANCODE_ESCAPE:
            *ov->cfg    = ov->saved;  /* revert */
            overlay_apply_crt(ov);
            ov->dirty   = false;
            ov->visible = false;
            break;
        default:
            break;
        }
        return true;
    }

    /* ---- ROM slots sub-panel ---- */
    /* idx 0 = Lower ROM; idx 1-32 = upper slots 0-31 */
    if (ov->state == OV_STATE_ROMSLOTS) {
        /* Inline board-tag editor: when active, every key flows into
         * the edit buffer. Enter commits, Esc cancels. */
        if (ov->romslot_editing) {
            if (sc == SDL_SCANCODE_ESCAPE) {
                ov->romslot_editing = false;
            } else if (sc == SDL_SCANCODE_RETURN
                    || sc == SDL_SCANCODE_KP_ENTER) {
                int slot = ov->romslot_row - 1;
                if (slot >= 0 && slot < ROM_EXT_COUNT) {
                    char prev_csv[64];
                    snprintf(prev_csv, sizeof(prev_csv), "%s",
                             ov->cfg->rom_ext_boards[slot]);
                    config_normalize_boards(ov->romslot_edit_buf,
                        ov->cfg->rom_ext_boards[slot],
                        sizeof(ov->cfg->rom_ext_boards[slot]));
                    /* Sync the per-board template tables: for each board
                     * now tagged, store this slot's current path; for
                     * each board removed, clear the template entry. */
                    for (int b = 0; b < CONFIG_BOARDS_COUNT; b++) {
                        const char *board = CONFIG_BOARDS[b];
                        char (*tbl)[CONFIG_PATH_MAX] =
                            config_board_slots(ov->cfg, board);
                        if (!tbl) continue;
                        bool was = config_boards_contains(prev_csv, board);
                        bool now = config_boards_contains(
                            ov->cfg->rom_ext_boards[slot], board);
                        if (now && ov->cfg->rom_ext[slot][0]) {
                            snprintf(tbl[slot], sizeof(tbl[slot]),
                                     "%s", ov->cfg->rom_ext[slot]);
                        } else if (was && !now) {
                            tbl[slot][0] = '\0';
                        }
                    }
                    ov->dirty = true;
                    /* If the tag change touches an already-enabled
                     * board, the slot may need reload/clear — let
                     * apply_boards run and flag cold-boot if so. */
                    if (config_apply_boards(ov->cfg) > 0)
                        ov->needs_cold_boot = true;
                }
                ov->romslot_editing = false;
            } else if (sc == SDL_SCANCODE_BACKSPACE) {
                if (ov->romslot_edit_len > 0)
                    ov->romslot_edit_buf[--ov->romslot_edit_len] = '\0';
            } else {
                /* Accept letters a–z, digits 0–9, comma, space.
                 * Only `m4` needs digits from the whitelist but we
                 * accept all for future-proofing. */
                char ch = 0;
                if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
                    ch = (char)('a' + (sc - SDL_SCANCODE_A));
                else if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
                    ch = (char)('1' + (sc - SDL_SCANCODE_1));
                else if (sc == SDL_SCANCODE_0)
                    ch = '0';
                else if (sc == SDL_SCANCODE_COMMA)
                    ch = ',';
                else if (sc == SDL_SCANCODE_SPACE)
                    ch = ' ';
                if (ch && ov->romslot_edit_len + 1 <
                          (int)sizeof(ov->romslot_edit_buf)) {
                    ov->romslot_edit_buf[ov->romslot_edit_len++] = ch;
                    ov->romslot_edit_buf[ov->romslot_edit_len] = '\0';
                }
            }
            return true;
        }
        switch (sc) {
        case SDL_SCANCODE_ESCAPE:
            ov->state = OV_STATE_MENU;
            break;
        case SDL_SCANCODE_INSERT: {
            if (ov->romslot_row == 0) break;  /* lower ROM not taggable */
            int slot = ov->romslot_row - 1;
            if (!ov->cfg->rom_ext[slot][0]) break;  /* empty slot — no-op */
            ov->romslot_editing = true;
            snprintf(ov->romslot_edit_buf, sizeof(ov->romslot_edit_buf),
                     "%s", ov->cfg->rom_ext_boards[slot]);
            ov->romslot_edit_len = (int)strlen(ov->romslot_edit_buf);
            break;
        }
        case SDL_SCANCODE_UP:
            if (ov->romslot_row > 0) {
                ov->romslot_row--;
                if (ov->romslot_row < ov->romslot_scroll)
                    ov->romslot_scroll = ov->romslot_row;
            }
            break;
        case SDL_SCANCODE_DOWN:
            if (ov->romslot_row < ROMSLOT_TOTAL - 1) {
                ov->romslot_row++;
                if (ov->romslot_row >= ov->romslot_scroll + ROMSLOT_VISIBLE)
                    ov->romslot_scroll = ov->romslot_row - ROMSLOT_VISIBLE + 1;
            }
            break;
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: {
            static const SDL_DialogFileFilter filters[] = {
                { "ROM images", "rom;ROM" },
                { "All files",  "*"       },
            };
            if (ov->romslot_row == 0) {
                ov->dialog_kind = DIALOG_LOWER_ROM;
            } else {
                ov->dialog_kind = DIALOG_ROMSLOT;
                ov->dialog_slot = ov->romslot_row - 1;
            }
            ov->dialog_ready = false;
            SDL_ShowOpenFileDialog(overlay_file_callback, ov,
                ov->cpc ? ov->cpc->display.window : NULL,
                filters, 2, ov->cfg->last_dir[0] ? ov->cfg->last_dir : NULL, false);
            break;
        }
        case SDL_SCANCODE_DELETE:
        case SDL_SCANCODE_BACKSPACE:
            if (ov->romslot_row == 0) {
                /* Lower ROM: restore model default OS */
                config_default_os(ov->cfg->model,
                    ov->cfg->rom_os, sizeof(ov->cfg->rom_os));
                if (ov->cpc)
                    mem_load_os(&ov->cpc->mem, ov->cfg->rom_os);
                ov->needs_cold_boot = true;
                ov->dirty = true;
            } else {
                int slot = ov->romslot_row - 1;
                /* Clear any expansion override first */
                ov->cfg->rom_ext[slot][0] = '\0';
                if (ov->cpc)
                    mem_unload_rom_ext(&ov->cpc->mem, slot);
                /* For slot 0 (BASIC) or slot 7 (AMSDOS) restore the default */
                if (slot == 0) {
                    config_default_basic(ov->cfg->model,
                        ov->cfg->rom_basic, sizeof(ov->cfg->rom_basic));
                } else if (slot == 7) {
                    config_default_amsdos(ov->cfg->model,
                        ov->cfg->rom_amsdos, sizeof(ov->cfg->rom_amsdos));
                }
                /* Removing the ROM also removes its board membership and
                 * the per-board template entries that pointed at it —
                 * otherwise the next enable of any tagged board would
                 * silently restore the now-deleted path on cold boot,
                 * and the [board:NAME] sections would still reference
                 * the dropped slot in the saved conf. */
                if (ov->cfg->rom_ext_boards[slot][0]) {
                    for (int b = 0; b < CONFIG_BOARDS_COUNT; b++) {
                        char (*tbl)[CONFIG_PATH_MAX] =
                            config_board_slots(ov->cfg, CONFIG_BOARDS[b]);
                        if (tbl) tbl[slot][0] = '\0';
                    }
                    ov->cfg->rom_ext_boards[slot][0] = '\0';
                }
                ov->needs_cold_boot = true;
                ov->dirty = true;
            }
            break;
        default:
            break;
        }
        return true;
    }

    /* ---- Normal menu navigation ---- */
    switch (sc) {
    case SDL_SCANCODE_ESCAPE:
        try_close(ov);
        break;
    case SDL_SCANCODE_LEFT:
        do { ov->section = (OvSection)((ov->section + OV_SEC_COUNT - 1) % OV_SEC_COUNT); }
        while ((ov->section == OV_ADVANCED && !ov->cfg->mx4) ||
               (ov->section == OV_TINKER   && !ov->cfg->tinker));
        ov->row = 0;
        break;
    case SDL_SCANCODE_RIGHT:
        do { ov->section = (OvSection)((ov->section + 1) % OV_SEC_COUNT); }
        while ((ov->section == OV_ADVANCED && !ov->cfg->mx4) ||
               (ov->section == OV_TINKER   && !ov->cfg->tinker));
        ov->row = 0;
        break;
    case SDL_SCANCODE_UP:
        {
            int n = ov_section_rows(ov, ov->section);
            ov->row = (ov->row + n - 1) % n;
        }
        break;
    case SDL_SCANCODE_DOWN:
        ov->row = (ov->row + 1) % ov_section_rows(ov, ov->section);
        break;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
        activate_item(ov, ev->key.mod);
        overlay_check_board_changes(ov);
        break;
    case SDL_SCANCODE_N:
        /* N on Drive A / Drive B pops a save-as dialog and creates a
         * fresh blank CPC DATA-format .dsk at the chosen path, then
         * inserts it into the drive. */
        if (ov->section == OV_STORAGE &&
            (ov->row == 0 || ov->row == 1) && floppy_accessible(ov)) {
            ov->dialog_kind  = DIALOG_DISK_NEW;
            ov->dialog_drive = ov->row;
            ov->dialog_ready = false;
            static const SDL_DialogFileFilter dsk_filters[] = {
                { "DSK images", "dsk;DSK" },
                { "All files",  "*"       },
            };
            SDL_ShowSaveFileDialog(overlay_file_callback, ov,
                ov->cpc ? ov->cpc->display.window : NULL,
                dsk_filters, 2, ov->cfg->last_dir[0] ? ov->cfg->last_dir : NULL);
        }
        break;
    case SDL_SCANCODE_DELETE:
    case SDL_SCANCODE_BACKSPACE:
        if (reset_tinker_item(ov))
            break;

        /* Del on Drive A / Drive B / Tape ejects the image and clears
         * the cached path (#107 follow-up). */
        if (ov->section == OV_STORAGE) {
            if ((ov->row == 0 || ov->row == 1) && floppy_accessible(ov)) {
                int drv = ov->row;
                char *dest = (drv == 0) ? ov->cfg->disk_a : ov->cfg->disk_b;
                if (dest[0]) {
                    dest[0] = '\0';
                    if (ov->cpc) disk_eject(&ov->cpc->drive[drv]);
                    ov->dirty = true;
                }
            } else if (ov->row == 2 && ov->cfg->tape[0]) {
                ov->cfg->tape[0] = '\0';
                ov->dirty = true;
            }
            break;
        }
        /* Del on the Extensions tab row for M4 / Symbiface IDE /
         * Albireo also clears the cached board image — so the next
         * enable re-prompts for a fresh path. Toggles the extension
         * off as a side effect (the live cfg field is cleared too). */
        if (ov->section == OV_ADVANCED) {
            char *cached = NULL;
            switch (ov->row) {
            case 0:                                /* M4 */
                cached = ov->cfg->board_m4_image;
                ov->cfg->m4 = false;
                ov->cfg->m4_image[0] = '\0';
                if (ov->cpc) {
                    ov->cpc->m4 = false;
                    m4_set_image(&ov->cpc->m4_card, "");
                }
                break;
            case 7:                                /* Symbiface IDE (cyboard) */
                cached = ov->cfg->board_cyboard_image;
                ov->cfg->symbiface_ide = false;
                ov->cfg->ide_image[0] = '\0';
                break;
            case 10:                               /* Albireo / dual-CH376 */
                cached = ov->cfg->board_albireo_image;
                ov->cfg->albireo = false;
                ov->cfg->albireo_image[0] = '\0';
                ov->cfg->albireo_mouse = false;
                if (ov->cpc) {
                    ov->cpc->albireo = false;
                    ov->cpc->albireo_mouse = false;
                    ov->cpc->ch376.has_mouse = false;
                    ch376_close(&ov->cpc->ch376);
                    ch376_close(&ov->cpc->ch376_b);
                }
                break;
            }
            if (cached) {
                cached[0] = '\0';
                ov->dirty = true;
                ov->needs_cold_boot = true;
                overlay_check_board_changes(ov);
            }
        }
        break;
    default:
        break;
    }
    return true;
}

static void fit_browser_text(char *dst, size_t cap, const char *src,
                             size_t max_chars, bool keep_tail) {
    size_t len = strlen(src);
    if (len <= max_chars) {
        snprintf(dst, cap, "%s", src);
        return;
    }
    if (max_chars < 4) {
        snprintf(dst, cap, "%.*s", (int)max_chars, src);
        return;
    }
    if (keep_tail)
        snprintf(dst, cap, "...%s", src + len - (max_chars - 3));
    else
        snprintf(dst, cap, "%.*s...", (int)(max_chars - 3), src);
}

static void render_file_browser(const Overlay *ov, SDL_Renderer *r,
                                float lw, float lh) {
    float box_w = lw - 16.0f;
    if (box_w > 496.0f) box_w = 496.0f;
    float box_h = 344.0f;
    if (box_h > lh - 16.0f) box_h = lh - 16.0f;
    float bx = (lw - box_w) / 2.0f;
    float by = (lh - box_h) / 2.0f;
    float list_y = by + 46.0f;

    fill_rect(r, 0, 0, lw, lh, 0, 0, 0, 170);
    fill_rect(r, bx, by, box_w, box_h, 15, 15, 40, 255);
    draw_rect_outline(r, bx, by, box_w, box_h, 70, 90, 200);

    char title[64];
    snprintf(title, sizeof(title), "Select DSK image for Drive %c",
             ov->browser_drive == 0 ? 'A' : 'B');
    draw_text(r, bx + 10.0f, by + 7.0f, title, 255, 255, 120);

    char shown_path[64];
    fit_browser_text(shown_path, sizeof(shown_path), ov->browser_dir,
                     57, true);
    draw_text(r, bx + 10.0f, by + 24.0f, shown_path, 170, 205, 230);

    for (int shown = 0; shown < BROWSER_VISIBLE_ROWS; shown++) {
        int index = ov->browser_scroll + shown;
        if (index >= ov->browser_entry_count) break;
        const OverlayBrowserEntry *entry = &ov->browser_entries[index];
        float y = list_y + (float)(shown * BROWSER_ROW_H);
        bool selected = index == ov->browser_row;
        if (selected)
            fill_rect(r, bx + 7.0f, y - 2.0f, box_w - 14.0f,
                      13.0f, 70, 90, 200, 255);

        char line[64];
        char name[54];
        fit_browser_text(name, sizeof(name), entry->name, 50, false);
        if (entry->parent)
            snprintf(line, sizeof(line), "[UP]  %s", name);
        else if (entry->directory)
            snprintf(line, sizeof(line), "[DIR] %s", name);
        else
            snprintf(line, sizeof(line), "      %s", name);
        draw_text(r, bx + 12.0f, y, line,
                  selected ? 255 : 215,
                  selected ? 255 : 220,
                  selected ? 120 : 225);
    }

    float status_y = list_y + BROWSER_VISIBLE_ROWS * BROWSER_ROW_H + 2.0f;
    if (ov->browser_error[0]) {
        char error[64];
        fit_browser_text(error, sizeof(error), ov->browser_error, 57, false);
        draw_text(r, bx + 10.0f, status_y, error, 255, 130, 130);
    } else if (ov->browser_entry_count == 0) {
        draw_text(r, bx + 10.0f, status_y,
                  "No directories or DSK images", 150, 150, 175);
    } else {
        char count[64];
        snprintf(count, sizeof(count), "%d item%s",
                 ov->browser_entry_count,
                 ov->browser_entry_count == 1 ? "" : "s");
        draw_text(r, bx + 10.0f, status_y, count, 140, 150, 175);
    }
    draw_text(r, bx + 10.0f, status_y + 17.0f,
              "Enter: open/select  Backspace/Left: up  Esc: cancel",
              180, 185, 200);
}

void overlay_render(const Overlay *ov, SDL_Renderer *r) {
    if (!ov->visible) return;

    int rw, rh;
    SDL_GetRenderOutputSize(r, &rw, &rh);
    float lw = rw / SCALE;
    float lh = rh / SCALE;

    SDL_SetRenderScale(r, SCALE, SCALE);

    /* ---- ROM slots sub-panel ---- */
    if (ov->state == OV_STATE_ROMSLOTS) {
        fill_rect(r, 0, 0, lw, lh, 10, 10, 30, 245);
        draw_text(r, DROP_PAD, 4, "ROMs  Esc=back  Enter=load  Del=clear  Ins=tag (m4/albireo/cyboard)",
                  180, 180, 220);
        SDL_SetRenderDrawColor(r, 70, 90, 200, 255);
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        SDL_RenderLine(r, 0, BAR_H - 1, lw, BAR_H - 1);

        for (int i = 0; i < ROMSLOT_VISIBLE; i++) {
            int idx = ov->romslot_scroll + i;
            if (idx >= ROMSLOT_TOTAL) break;
            float iy = BAR_H + 2.0f + i * ITEM_H;
            bool sel = (idx == ov->romslot_row);

            if (sel)
                fill_rect(r, 0, iy, lw, ITEM_H, 70, 90, 200, 255);

            float ty = iy + (ITEM_H - FONT_H) / 2.0f;
            char val[48] = "";

            if (idx == 0) {
                /* Lower ROM — always green */
                draw_text(r, DROP_PAD, ty, "Lower ROM", 220, 220, 240);
                char tmp[CONFIG_PATH_MAX];
                snprintf(tmp, sizeof(tmp), "%s", ov->cfg->rom_os);
                trunc_path(basename(tmp), val, sizeof(val));
                draw_text(r, VAL_X, ty, val, 80, 220, 80);
            } else {
                int slot = idx - 1;
                char lbl[24];
                snprintf(lbl, sizeof(lbl), "Slot %2d", slot);
                draw_text(r, DROP_PAD, ty, lbl, 220, 220, 240);

                bool has_ext = ov->cfg->rom_ext[slot][0] != '\0';
                if (slot == 0) {
                    /* BASIC slot — red */
                    if (has_ext) {
                        char tmp[CONFIG_PATH_MAX];
                        snprintf(tmp, sizeof(tmp), "%s", ov->cfg->rom_ext[slot]);
                        trunc_path(basename(tmp), val, sizeof(val));
                    } else {
                        snprintf(val, sizeof(val), "(BASIC)");
                    }
                    draw_text(r, VAL_X, ty, val, 220, 80, 80);
                } else if (slot == 7) {
                    /* AMSDOS slot — yellow */
                    if (has_ext) {
                        char tmp[CONFIG_PATH_MAX];
                        snprintf(tmp, sizeof(tmp), "%s", ov->cfg->rom_ext[slot]);
                        trunc_path(basename(tmp), val, sizeof(val));
                    } else {
                        snprintf(val, sizeof(val), "(AMSDOS)");
                    }
                    draw_text(r, VAL_X, ty, val, 220, 200, 60);
                } else if (has_ext) {
                    /* Other populated slot — white */
                    char tmp[CONFIG_PATH_MAX];
                    snprintf(tmp, sizeof(tmp), "%s", ov->cfg->rom_ext[slot]);
                    trunc_path(basename(tmp), val, sizeof(val));
                    draw_text(r, VAL_X, ty, val, 220, 220, 220);
                } else {
                    /* Empty slot — grey */
                    draw_text(r, VAL_X, ty, "[empty]", 70, 70, 90);
                }

                /* Boards column — show the comma-separated board CSV
                 * (m4/albireo/cyboard) the slot is tagged with, or
                 * (when the user is editing this slot) the live edit
                 * buffer with a cursor. */
                #define BOARDS_X 360
                bool editing_here = (ov->state == OV_STATE_ROMSLOTS
                                     && ov->romslot_editing
                                     && idx == ov->romslot_row);
                if (editing_here) {
                    char buf[80];
                    snprintf(buf, sizeof(buf), "%s_", ov->romslot_edit_buf);
                    draw_text(r, BOARDS_X, ty, buf, 255, 220, 80);
                } else if (ov->cfg->rom_ext_boards[slot][0] && has_ext) {
                    draw_text(r, BOARDS_X, ty,
                              ov->cfg->rom_ext_boards[slot],
                              150, 200, 255);
                }
                #undef BOARDS_X
            }
        }
        SDL_SetRenderScale(r, 1.0f, 1.0f);
        return;
    }

    /* ---- Top bar ---- */
    fill_rect(r, 0, 0, lw, BAR_H, 20, 20, 50, 230);

    /* Tabs are laid out left-to-right; hidden tabs collapse the gap. */
    float tx = (float)sec_x[0];
    for (int i = 0; i < OV_SEC_COUNT; i++) {
        /* Extensions tab is hidden entirely when MX4 is disabled. */
        if (i == OV_ADVANCED && !ov->cfg->mx4) continue;
        if (i == OV_TINKER   && !ov->cfg->tinker) continue;
        bool sel = (ov->section == (OvSection)i);
        float ty = (BAR_H - FONT_H) / 2.0f;
        float lw_tab = strlen(sec_labels[i]) * FONT_W;

        if (sel) {
            fill_rect(r, tx - 2, 1, lw_tab + 4.0f, BAR_H - 2, 70, 90, 200, 255);
            draw_text(r, tx, ty, sec_labels[i], 255, 255, 255);
        } else {
            draw_text(r, tx, ty, sec_labels[i], 150, 150, 175);
        }
        tx += lw_tab + 16.0f;   /* gutter between tabs */
    }

    /* ---- Dropdown ---- */
    int nrows = ov_section_rows(ov, ov->section);
    float drop_h = nrows * ITEM_H + 4.0f;
    fill_rect(r, 0, BAR_H, lw, drop_h, 15, 15, 40, 245);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 70, 90, 200, 255);
    SDL_RenderLine(r, 0, BAR_H + drop_h, lw, BAR_H + drop_h);

    for (int i = 0; i < nrows; i++) {
        float iy = BAR_H + 2.0f + i * ITEM_H;
        bool sel = (i == ov->row);

        if (sel)
            fill_rect(r, 0, iy, lw, ITEM_H, 70, 90, 200, 255);

        char lbl[48] = "", val[48] = "";
        bool readonly;
        item_text(ov, i, lbl, sizeof(lbl), val, sizeof(val), &readonly);
        int logical = (ov->section == OV_TINKER)
                    ? tinker_logical_row(ov, i) : i;

        float ty = iy + (ITEM_H - FONT_H) / 2.0f;
        draw_text(r, DROP_PAD, ty, lbl, 220, 220, 240);
        if (ov->section == OV_TINKER && logical == 12 && ov->pty_link_editing) {
            /* Inline editor: show the live buffer with a blinking-style
             * trailing underscore cursor. Bright green to signal that
             * keystrokes are being captured. */
            char shown[CONFIG_PATH_MAX + 2];
            snprintf(shown, sizeof(shown), "%s_", ov->pty_link_edit_buf);
            draw_text(r, VAL_X, ty, shown, 120, 255, 120);
        } else if (readonly) {
            draw_text(r, VAL_X, ty, val, 90, 90, 110);
        } else if (ov->section == OV_TINKER && logical == 13
                   && ov->cfg->monochrome != MONO_OFF) {
            /* Render the tint name in its own phosphor colour so the
             * choice is recognisable at a glance. Bright values picked
             * to read against both the selected-row blue and the bar. */
            Uint8 R = 255, G = 200, B = 50;
            switch (ov->cfg->monochrome) {
                case MONO_GREEN: R =  90; G = 255; B =  90; break;
                case MONO_AMBER: R = 255; G = 191; B =   0; break;
                case MONO_WHITE: R = 255; G = 255; B = 255; break;
                default: break;
            }
            draw_text(r, VAL_X, ty, val, R, G, B);
        } else {
            draw_text(r, VAL_X, ty, val, 255, 200, 50);
        }
    }

    if (ov->section == OV_STORAGE) {
        draw_text(r, DROP_PAD, BAR_H + drop_h + 7.0f,
                  ov->sdl_fm
                      ? "Enter: built-in  N:new  Del:eject"
                      : "Enter: system  Shift+Enter: built-in  N:new  Del:eject",
                  150, 150, 175);
    }

    if (ov->state == OV_STATE_FILE_BROWSER) {
        render_file_browser(ov, r, lw, lh);
        SDL_SetRenderScale(r, 1.0f, 1.0f);
        return;
    }

    /* ---- Confirm dialog ---- */
    if (ov->state == OV_STATE_CONFIRM) {
        /* Dim everything behind the dialog */
        fill_rect(r, 0, 0, lw, lh, 0, 0, 0, 140);

        const char *line1 = "Save changes?";
        const char *line2 = "Enter = Save      Esc = Discard";
        int l1w = strlen(line1) * FONT_W;
        int l2w = strlen(line2) * FONT_W;
        int box_w = l2w + 24;
        int box_h = FONT_H * 2 + 24;
        float bx = (lw - box_w) / 2.0f;
        float by = (lh - box_h) / 2.0f;

        fill_rect(r, bx, by, box_w, box_h, 25, 25, 60, 255);
        draw_rect_outline(r, bx, by, box_w, box_h, 70, 90, 200);

        draw_text(r, bx + (box_w - l1w) / 2.0f, by + 6,
                  line1, 255, 255, 255);
        draw_text(r, bx + (box_w - l2w) / 2.0f, by + 6 + FONT_H + 8,
                  line2, 200, 200, 100);
    }

    SDL_SetRenderScale(r, 1.0f, 1.0f);
}

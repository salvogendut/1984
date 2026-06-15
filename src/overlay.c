#include "overlay.h"
#include "cpc.h"
#include "tap.h"     /* TAP_SUPPORTED */
#include "m4.h"
#include "disk.h"
#include "mem.h"
#include "snapshot.h"
#include "webmcap.h"   /* WEBMCAP_SUPPORTED */
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

static const char *const sec_labels[OV_SEC_COUNT] = {
    "General", "Media", "Extensions", "Advanced"
};
static const int sec_x[OV_SEC_COUNT] = { 8, 80, 160, 248 };
/* General has 7 rows on CPC 464, 8 on 6128 (the extra one is the
 * "External Tape" toggle, only meaningful on the 6128 since the 464 has
 * the cassette deck built in). Other sections are fixed.
 * The Advanced tab (OV_TINKER) is hidden unless cfg->tinker is enabled. */
static const int sec_row_count[OV_SEC_COUNT] = { 7, 3, 12, 9 };

static int ov_section_rows(const Overlay *ov, OvSection s) {
    if (s == OV_GENERAL && ov->cfg->model == MODEL_6128) return 8;
    return sec_row_count[s];
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

/* True when floppy drives are accessible (6128 always; 464 only with DD1) */
static bool floppy_accessible(const Overlay *ov) {
    return ov->cfg->model == MODEL_6128 || ov->cfg->dd1;
}

/* ---- Menu item text ---- */

static void item_text(const Overlay *ov, int row,
                      char *lbl, size_t lsz,
                      char *val, size_t vsz,
                      bool *readonly) {
    *readonly = false;

    switch (ov->section) {

    case OV_GENERAL: {
        bool is_6128 = (ov->cfg->model == MODEL_6128);
        /* On 6128 the rows are: Model, Memory, MX4, Roms Board, External
         * Tape, OS ROM, BASIC ROM. On 464 the External Tape row is
         * hidden (464 has a built-in deck — always available); the ROM
         * rows then shift up by one. */
        int logical = row;
        if (!is_6128 && row >= 4) logical = row + 1;   /* skip slot 4 */
        switch (logical) {
        case 0:
            snprintf(lbl, lsz, "Model");
            snprintf(val, vsz, "%s",
                ov->cfg->model == MODEL_464 ? "CPC 464" : "CPC 6128");
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
                char tmp[CONFIG_PATH_MAX];
                snprintf(tmp, sizeof(tmp), "%s", ov->cfg->disk_a);
                trunc_path(basename(tmp), val, vsz);
            }
            else
                snprintf(val, vsz, "[empty]  Enter=load");
            break;
        case 1:
            snprintf(lbl, lsz, "Drive B");
            if (!accessible) {
                snprintf(val, vsz, "[enable DD1 in Advanced]");
                *readonly = true;
            } else if (db && db->inserted && ov->cfg->disk_b[0]) {
                char tmp[CONFIG_PATH_MAX];
                snprintf(tmp, sizeof(tmp), "%s", ov->cfg->disk_b);
                trunc_path(basename(tmp), val, vsz);
            }
            else
                snprintf(val, vsz, "[empty]  Enter=load");
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
            snprintf(lbl, lsz, "M4 (experimental)");
            if (!ov->cfg->rom_board) {
                snprintf(val, vsz, "[needs Roms Board]");
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
            snprintf(lbl, lsz, "UliFAC");
            snprintf(val, vsz, "[unimplemented]");
            *readonly = true;
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
            if (ov->cfg->model == MODEL_6128) {
                snprintf(val, vsz, "N/A (6128 has built-in FDC)");
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
            if (!ov->cfg->rom_board) {
                snprintf(val, vsz, "[needs Roms Board]");
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
            if (!ov->cfg->rom_board) {
                snprintf(val, vsz, "[needs Roms Board]");
                *readonly = true;
            } else {
                snprintf(val, vsz, "%s", ov->cfg->symbiface_mouse ? "enabled" : "disabled");
            }
            break;
        case 9:
            snprintf(lbl, lsz, "CH376-A Mouse");
            if (!ov->cfg->rom_board) {
                snprintf(val, vsz, "(Albireo compatible) [needs Roms Board]");
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
            if (!ov->cfg->rom_board) {
                snprintf(val, vsz, "(Albireo compatible) [needs Roms Board]");
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
            if (!ov->cfg->rom_board) {
                snprintf(val, vsz, "[needs Roms Board]");
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
        }
        break;

    case OV_TINKER:
        switch (row) {
        case 0:
            snprintf(lbl, lsz, "Smoothing");
            snprintf(val, vsz, "%s",
                     ov->cfg->fullscreen_smoothing ? "smooth" : "sharp");
            break;
        case 1:
            snprintf(lbl, lsz, "Real CRT");
            snprintf(val, vsz, "[unimplemented]");
            *readonly = true;
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

static void activate_item(Overlay *ov) {
    switch (ov->section) {

    case OV_GENERAL: {
        bool is_6128 = (ov->cfg->model == MODEL_6128);
        int logical = ov->row;
        if (!is_6128 && ov->row >= 4) logical = ov->row + 1;   /* skip slot 4 (External Tape) */
        switch (logical) {
        case 0: {
            CpcModel next = (ov->cfg->model == MODEL_464) ? MODEL_6128 : MODEL_464;
            config_set_model(ov->cfg, next);
            if (next == MODEL_6128 && ov->cfg->memory_kb < 128)
                ov->cfg->memory_kb = 128;
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
                rom_filters, 2, NULL, false);
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
                rom_filters, 2, NULL, false);
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
        }
        break;
    }

    case OV_STORAGE:
        if ((ov->row == 0 || ov->row == 1) && floppy_accessible(ov)) {
            ov->dialog_kind  = DIALOG_DISK;
            ov->dialog_drive = ov->row;
            ov->dialog_ready = false;
            static const SDL_DialogFileFilter filters[] = {
                { "DSK images", "dsk;DSK" },
                { "All files",  "*"       },
            };
            SDL_ShowOpenFileDialog(overlay_file_callback, ov,
                ov->cpc ? ov->cpc->display.window : NULL,
                filters, 2, NULL, false);
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
                tape_filters, 2, NULL, false);
        }
        break;

    case OV_ADVANCED:
        /* Without the Roms Board fitted, every ROM-backed expansion is
         * inert — block toggle on those rows. Net4CPC / RTC / DD1 / Diag
         * Cart / ROM Slots have their own gating; the rest are nailed
         * down here. */
        if (!ov->cfg->rom_board &&
            (ov->row == 0 || ov->row == 7 || ov->row == 8 ||
             ov->row == 9 || ov->row == 10 || ov->row == 11))
            break;
        switch (ov->row) {
        case 0:
            if (!ov->cfg->m4) {
                /* Mutually exclusive with Albireo (both claim port 0xFExx)
                 * and with the Cyboard RTC — M4ROM is incompatible with
                 * UNIDOS/Albireo tooling, so enabling M4 forces a clean
                 * scenario by disabling both. */
                if (ov->cfg->albireo) {
                    ov->cfg->albireo = false;
                    ov->cfg->albireo_image[0] = '\0';
                    if (ov->cpc) {
                        ov->cpc->albireo = false;
                        ch376_close(&ov->cpc->ch376);
                    }
                }
                /* Tear down the full Cyboard pack (matches what
                 * toggling Cyboard off does) — Net4CPC, RTC, IDE, Mouse. */
                if (ov->cfg->net4cpc) ov->cfg->net4cpc = false;
                if (ov->cfg->rtc) {
                    ov->cfg->rtc = false;
                    if (ov->cpc) ov->cpc->rtc = false;
                }
                if (ov->cfg->symbiface_ide) {
                    ov->cfg->symbiface_ide = false;
                    ov->cfg->ide_image[0]  = '\0';
                }
                if (ov->cfg->symbiface_mouse) ov->cfg->symbiface_mouse = false;
                /* Drop every non-default expansion ROM so nothing
                 * (UNIDOS, custom utilities, etc.) interferes with
                 * M4ROM. Also restore the stock BASIC + AMSDOS so slots
                 * 0 and 7 fall back to clean defaults. */
                for (int i = 0; i < ROM_EXT_COUNT; i++) {
                    if (!ov->cfg->rom_ext[i][0]) continue;
                    ov->cfg->rom_ext[i][0] = '\0';
                    if (ov->cpc) mem_unload_rom_ext(&ov->cpc->mem, i);
                }
                config_default_basic(ov->cfg->model,
                                     ov->cfg->rom_basic, sizeof(ov->cfg->rom_basic));
                if (!ov->cfg->rom_amsdos[0])
                    config_default_amsdos(ov->cfg->rom_amsdos,
                                          sizeof(ov->cfg->rom_amsdos));
                if (ov->cpc)
                    mem_load_amsdos(&ov->cpc->mem, ov->cfg->rom_amsdos);
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
                        m4_filters, 2, NULL, false);
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
        /* case 1 (UliFAC) is unimplemented — Enter does nothing */
        case 2:
            ov->cfg->net4cpc = !ov->cfg->net4cpc;
            ov->dirty = true;
            break;
        case 3:
            ov->cfg->rtc = !ov->cfg->rtc;
            /* Cyboard RTC is incompatible with M4ROM — enabling RTC
             * disables M4 for a clean scenario (mirrors the reverse
             * forced by case 0). */
            if (ov->cfg->rtc && ov->cfg->m4) {
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
                        ide_filters, 2, NULL, false);
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
            /* SymbIface Mouse and Albireo Mouse both drive the host
             * pointer — only one can own it at a time. */
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
            if (!ov->cfg->rom_board || !ov->cfg->albireo) break;
            ov->cfg->albireo_mouse = !ov->cfg->albireo_mouse;
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
                        alb_filters, 2, NULL, false);
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
            /* Cyboard pack is incompatible with M4ROM — enabling forces
             * a clean scenario by disabling M4 (mirrors the M4 enable
             * path in case 0). */
            if (enable && ov->cfg->m4) {
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
                        ide_filters, 2, NULL, false);
                    /* dirty set by file callback once image is chosen */
                }
            } else {
                ov->dirty = true;
            }
            break;
        }
        }
        break;

    case OV_TINKER:
        switch (ov->row) {
        case 0:
            ov->cfg->fullscreen_smoothing = !ov->cfg->fullscreen_smoothing;
            if (ov->cpc)
                display_set_smoothing(&ov->cpc->display,
                                      ov->cfg->fullscreen_smoothing);
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
                sna_filters, 2, NULL, false);
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
                sna_filters, 2, NULL);
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
                    webm_filters, 2, NULL);
            }
            break;
        }
        break;

    default:
        break;
    }
}

static void try_close(Overlay *ov) {
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
    if (files && files[0]) {
        snprintf(ov->dialog_path, sizeof(ov->dialog_path), "%s", files[0]);
        SDL_MemoryBarrierRelease();
        ov->dialog_ready = true;
    } else {
        ov->dialog_drive = -1;
        ov->dialog_slot  = -1;
        ov->dialog_kind  = DIALOG_NONE;
    }
}

void overlay_init(Overlay *ov, Config *cfg, CPC *cpc) {
    memset(ov, 0, sizeof(*ov));
    ov->cfg          = cfg;
    ov->cpc          = cpc;
    ov->dialog_kind  = DIALOG_NONE;
    ov->dialog_drive = -1;
    ov->dialog_slot  = -1;
    ov->last_m4            = cfg->m4;
    ov->last_albireo       = cfg->albireo;
    ov->last_symbiface_ide = cfg->symbiface_ide;
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

void overlay_tick(Overlay *ov) {
    if (!ov->dialog_ready) return;
    SDL_MemoryBarrierAcquire();
    ov->dialog_ready = false;

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
    } else if (ov->dialog_kind == DIALOG_TAPE) {
        /* Stub — just record the path. PSG cassette input not wired yet. */
        snprintf(ov->cfg->tape, CONFIG_PATH_MAX, "%s", ov->dialog_path);
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
            videocap_start(path);
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
                filters, 2, NULL, false);
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
                    config_default_amsdos(
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
        activate_item(ov);
        overlay_check_board_changes(ov);
        break;
    case SDL_SCANCODE_DELETE:
    case SDL_SCANCODE_BACKSPACE:
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

        float ty = iy + (ITEM_H - FONT_H) / 2.0f;
        draw_text(r, DROP_PAD, ty, lbl, 220, 220, 240);
        if (readonly)
            draw_text(r, VAL_X, ty, val, 90, 90, 110);
        else
            draw_text(r, VAL_X, ty, val, 255, 200, 50);
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

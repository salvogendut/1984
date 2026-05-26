#include "overlay.h"
#include "cpc.h"
#include "disk.h"
#include "mem.h"
#include <string.h>
#include <stdio.h>
#include <libgen.h>

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
    "General", "Storage", "Advanced"
};
static const int sec_x[OV_SEC_COUNT] = { 8, 74, 140 };
static const int sec_row_count[OV_SEC_COUNT] = { 3, 2, 6 };

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

    case OV_GENERAL:
        switch (row) {
        case 0:
            snprintf(lbl, lsz, "Model");
            snprintf(val, vsz, "%s",
                ov->cfg->model == MODEL_464 ? "CPC 464" : "CPC 6128");
            break;
        case 1:
            snprintf(lbl, lsz, "OS ROM");
            trunc_path(ov->cfg->rom_os, val, vsz);
            *readonly = true;
            break;
        case 2:
            snprintf(lbl, lsz, "BASIC ROM");
            trunc_path(ov->cfg->rom_basic, val, vsz);
            *readonly = true;
            break;
        }
        break;

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
            } else if (da && da->inserted && ov->cfg->disk_a[0])
                trunc_path(ov->cfg->disk_a, val, vsz);
            else
                snprintf(val, vsz, "[empty]  Enter=load");
            break;
        case 1:
            snprintf(lbl, lsz, "Drive B");
            if (!accessible) {
                snprintf(val, vsz, "[enable DD1 in Advanced]");
                *readonly = true;
            } else if (db && db->inserted && ov->cfg->disk_b[0])
                trunc_path(ov->cfg->disk_b, val, vsz);
            else
                snprintf(val, vsz, "[empty]  Enter=load");
            break;
        }
        break;
    }

    case OV_ADVANCED:
        switch (row) {
        case 0:
            snprintf(lbl, lsz, "Memory");
            snprintf(val, vsz, "%d KB", ov->cfg->memory_kb);
            break;
        case 1:
            snprintf(lbl, lsz, "M4");
            snprintf(val, vsz, "%s", ov->cfg->m4 ? "enabled" : "disabled");
            break;
        case 2:
            snprintf(lbl, lsz, "UliFAC");
            snprintf(val, vsz, "%s", ov->cfg->ulifac ? "enabled" : "disabled");
            break;
        case 3:
            snprintf(lbl, lsz, "Net4CPC");
            snprintf(val, vsz, "%s", ov->cfg->net4cpc ? "enabled" : "disabled");
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
            snprintf(val, vsz, "Enter to configure \xbb");
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

    case OV_GENERAL:
        if (ov->row == 0) {
            CpcModel next = (ov->cfg->model == MODEL_464) ? MODEL_6128 : MODEL_464;
            config_set_model(ov->cfg, next);
            ov->dirty = true;
        }
        break;

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
        }
        break;

    case OV_ADVANCED:
        switch (ov->row) {
        case 0:
            ov->cfg->memory_kb = (ov->cfg->memory_kb == 64) ? 128 : 64;
            ov->dirty = true;
            break;
        case 1:
            ov->cfg->m4 = !ov->cfg->m4;
            ov->dirty = true;
            break;
        case 2:
            ov->cfg->ulifac = !ov->cfg->ulifac;
            ov->dirty = true;
            break;
        case 3:
            ov->cfg->net4cpc = !ov->cfg->net4cpc;
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
            ov->state = OV_STATE_ROMSLOTS;
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

/* File-dialog callback — called from SDL's thread on some platforms */
static void overlay_file_callback(void *userdata, const char * const *files,
                                  int filter) {
    (void)filter;
    Overlay *ov = userdata;
    if (files && files[0]) {
        snprintf(ov->dialog_path, sizeof(ov->dialog_path), "%s", files[0]);
        ov->dialog_ready = true;
    } else {
        ov->dialog_drive = -1;
    }
}

void overlay_init(Overlay *ov, Config *cfg, CPC *cpc) {
    memset(ov, 0, sizeof(*ov));
    ov->cfg          = cfg;
    ov->cpc          = cpc;
    ov->dialog_kind  = DIALOG_NONE;
    ov->dialog_drive = -1;
    ov->dialog_slot  = -1;
}

void overlay_tick(Overlay *ov) {
    if (!ov->dialog_ready) return;
    ov->dialog_ready = false;

    if (ov->dialog_kind == DIALOG_DISK && ov->dialog_drive >= 0) {
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
    } else if (ov->dialog_kind == DIALOG_LOWER_ROM) {
        snprintf(ov->cfg->rom_os, CONFIG_PATH_MAX, "%s", ov->dialog_path);
        if (ov->cpc)
            mem_load_os(&ov->cpc->mem, ov->dialog_path);
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
            /* cold boot needed if model, DD1, or any ROM slot changed */
            bool boot = (ov->cfg->model != ov->saved.model) ||
                        (ov->cfg->dd1   != ov->saved.dd1)  ||
                        strcmp(ov->cfg->rom_os, ov->saved.rom_os);
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
        switch (sc) {
        case SDL_SCANCODE_ESCAPE:
            ov->state = OV_STATE_MENU;
            break;
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
        ov->section = (OvSection)((ov->section + OV_SEC_COUNT - 1) % OV_SEC_COUNT);
        ov->row = 0;
        break;
    case SDL_SCANCODE_RIGHT:
        ov->section = (OvSection)((ov->section + 1) % OV_SEC_COUNT);
        ov->row = 0;
        break;
    case SDL_SCANCODE_UP:
        ov->row = (ov->row + sec_row_count[ov->section] - 1)
                  % sec_row_count[ov->section];
        break;
    case SDL_SCANCODE_DOWN:
        ov->row = (ov->row + 1) % sec_row_count[ov->section];
        break;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER:
        activate_item(ov);
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
        draw_text(r, DROP_PAD, 4, "ROMs  Esc=back  Enter=load  Del=clear (upper slots)",
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
            }
        }
        SDL_SetRenderScale(r, 1.0f, 1.0f);
        return;
    }

    /* ---- Top bar ---- */
    fill_rect(r, 0, 0, lw, BAR_H, 20, 20, 50, 230);

    for (int i = 0; i < OV_SEC_COUNT; i++) {
        bool sel = (ov->section == (OvSection)i);
        float tx = sec_x[i];
        float ty = (BAR_H - FONT_H) / 2.0f;

        if (sel) {
            float hw = strlen(sec_labels[i]) * FONT_W + 4.0f;
            fill_rect(r, tx - 2, 1, hw, BAR_H - 2, 70, 90, 200, 255);
            draw_text(r, tx, ty, sec_labels[i], 255, 255, 255);
        } else {
            draw_text(r, tx, ty, sec_labels[i], 150, 150, 175);
        }
    }

    /* ---- Dropdown ---- */
    int nrows = sec_row_count[ov->section];
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

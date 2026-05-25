#include "overlay.h"
#include <string.h>
#include <stdio.h>

/* ---- Layout constants (logical pixels at 2× render scale) ---- */
#define FONT_W   SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE   /* 8 */
#define FONT_H   SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE   /* 8 */
#define BAR_H    20    /* top bar height */
#define ITEM_H   15    /* height of each dropdown row */
#define DROP_PAD  6    /* left margin inside dropdown */
#define VAL_X   140    /* x where values start inside dropdown */

static const char *const sec_labels[OV_SEC_COUNT] = {
    "General", "Storage", "Advanced"
};
static const int sec_x[OV_SEC_COUNT] = { 8, 108, 208 };
static const int sec_row_count[OV_SEC_COUNT] = { 3, 2, 4 };

/* ---- Helpers ---- */

static void fill_rect(SDL_Renderer *r, float x, float y, float w, float h,
                      Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
    SDL_SetRenderDrawBlendMode(r, A < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, R, G, B, A);
    SDL_FRect rect = { x, y, w, h };
    SDL_RenderFillRect(r, &rect);
}

static void trunc_path(const char *path, char *out, size_t sz) {
    size_t len = strlen(path);
    if (len < sz) {
        snprintf(out, sz, "%s", path);
    } else {
        size_t keep = sz - 4;   /* room for "..." + NUL */
        snprintf(out, sz, "...%s", path + len - keep);
    }
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

    case OV_STORAGE:
        switch (row) {
        case 0:
            snprintf(lbl, lsz, "Drive A");
            snprintf(val, vsz, "[not implemented]");
            *readonly = true;
            break;
        case 1:
            snprintf(lbl, lsz, "Tape");
            snprintf(val, vsz, "[not implemented]");
            *readonly = true;
            break;
        }
        break;

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
        }
        break;

    default:
        break;
    }
}

/* ---- Value cycling (Enter key) ---- */

static void activate_item(Overlay *ov) {
    switch (ov->section) {

    case OV_GENERAL:
        if (ov->row == 0) {
            ov->cfg->model = (ov->cfg->model == MODEL_464) ? MODEL_6128 : MODEL_464;
            config_save(ov->cfg);
        }
        break;

    case OV_STORAGE:
        break;

    case OV_ADVANCED:
        switch (ov->row) {
        case 0:
            ov->cfg->memory_kb = (ov->cfg->memory_kb == 64) ? 128 : 64;
            config_save(ov->cfg);
            break;
        case 1:
            ov->cfg->m4 = !ov->cfg->m4;
            config_save(ov->cfg);
            break;
        case 2:
            ov->cfg->ulifac = !ov->cfg->ulifac;
            config_save(ov->cfg);
            break;
        case 3:
            ov->cfg->net4cpc = !ov->cfg->net4cpc;
            config_save(ov->cfg);
            break;
        }
        break;

    default:
        break;
    }
}

/* ---- Public API ---- */

void overlay_init(Overlay *ov, Config *cfg) {
    memset(ov, 0, sizeof(*ov));
    ov->cfg = cfg;
}

bool overlay_handle_event(Overlay *ov, SDL_Event *ev) {
    if (ev->type != SDL_EVENT_KEY_DOWN) return false;

    SDL_Scancode sc = ev->key.scancode;

    if (sc == SDL_SCANCODE_F9) {
        ov->visible = !ov->visible;
        return true;
    }

    if (!ov->visible) return false;

    switch (sc) {
    case SDL_SCANCODE_ESCAPE:
        ov->visible = false;
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
    return true;    /* consume all keys while overlay is open */
}

void overlay_render(const Overlay *ov, SDL_Renderer *r) {
    if (!ov->visible) return;

    int rw, rh;
    SDL_GetRenderOutputSize(r, &rw, &rh);
    float lw = rw / 2.0f;   /* logical width at 2× scale */

    SDL_SetRenderScale(r, 2.0f, 2.0f);

    /* ---- Top bar background ---- */
    fill_rect(r, 0, 0, lw, BAR_H, 20, 20, 50, 230);

    /* ---- Section labels ---- */
    for (int i = 0; i < OV_SEC_COUNT; i++) {
        bool sel = (ov->section == (OvSection)i);
        float tx = sec_x[i];
        float ty = (BAR_H - FONT_H) / 2.0f;

        if (sel) {
            float hw = strlen(sec_labels[i]) * FONT_W + 4.0f;
            fill_rect(r, tx - 2, 1, hw, BAR_H - 2, 70, 90, 200, 255);
            SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        } else {
            SDL_SetRenderDrawColor(r, 150, 150, 175, 255);
        }
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        SDL_RenderDebugText(r, tx, ty, sec_labels[i]);
    }

    /* ---- Dropdown background ---- */
    int nrows = sec_row_count[ov->section];
    float drop_h = nrows * ITEM_H + 4.0f;
    fill_rect(r, 0, BAR_H, lw, drop_h, 15, 15, 40, 245);

    /* Bottom border */
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, 70, 90, 200, 255);
    SDL_RenderLine(r, 0, BAR_H + drop_h, lw, BAR_H + drop_h);

    /* ---- Dropdown items ---- */
    for (int i = 0; i < nrows; i++) {
        float iy = BAR_H + 2.0f + i * ITEM_H;
        bool sel = (i == ov->row);

        if (sel)
            fill_rect(r, 0, iy, lw, ITEM_H, 70, 90, 200, 255);

        char lbl[48] = "", val[48] = "";
        bool readonly;
        item_text(ov, i, lbl, sizeof(lbl), val, sizeof(val), &readonly);

        float ty = iy + (ITEM_H - FONT_H) / 2.0f;

        /* Label — white */
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(r, 220, 220, 240, 255);
        SDL_RenderDebugText(r, DROP_PAD, ty, lbl);

        /* Value — amber if editable, dim if read-only */
        if (readonly)
            SDL_SetRenderDrawColor(r, 90, 90, 110, 255);
        else
            SDL_SetRenderDrawColor(r, 255, 200, 50, 255);
        SDL_RenderDebugText(r, VAL_X, ty, val);
    }

    SDL_SetRenderScale(r, 1.0f, 1.0f);
}

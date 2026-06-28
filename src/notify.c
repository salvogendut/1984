#include "notify.h"

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* Logical render scale — must match overlay.c's SCALE so toasts share the
 * 1.5x logical-pixel coordinate space the rest of the on-screen UI uses. */
#define NOTIFY_SCALE     1.5f

#define NOTIFY_MAX       5
#define NOTIFY_TEXT_MAX  96
#define NOTIFY_TTL_MS    3500
#define NOTIFY_FADE_MS   600
#define LINE_H           14
#define PAD_X            8
#define PAD_Y            4
#define MARGIN           10
/* Clear the persistent bottom status bar (LED bar 24px + hint strip 16px =
 * 40 window px) so toasts stack just above it. In 1.5x logical space that is
 * 40 / 1.5 ~= 27px; round up and add MARGIN of breathing room. */
#define BOTTOM_INSET     (28 + MARGIN)

typedef struct {
    char text[NOTIFY_TEXT_MAX];
    int  age_ms;          /* -1 = empty slot */
} NotifyEntry;

static NotifyEntry g_slots[NOTIFY_MAX];
static NotifyMode  g_mode = NOTIFY_MODE_SCREEN;

void notify_init(void) {
    for (int i = 0; i < NOTIFY_MAX; i++) g_slots[i].age_ms = -1;
    g_mode = NOTIFY_MODE_SCREEN;
}

void notify_set_mode(NotifyMode mode) { g_mode = mode; }

static int oldest_slot(void) {
    int best = 0;
    for (int i = 1; i < NOTIFY_MAX; i++)
        if (g_slots[i].age_ms > g_slots[best].age_ms) best = i;
    return best;
}

void notify_post(const char *fmt, ...) {
    if (g_mode == NOTIFY_MODE_OFF) return;

    char buf[NOTIFY_TEXT_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (g_mode == NOTIFY_MODE_CONSOLE) {
        fprintf(stderr, "%s\n", buf);
        return;
    }

    int slot = -1;
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (g_slots[i].age_ms < 0) { slot = i; break; }
    }
    if (slot < 0) slot = oldest_slot();

    snprintf(g_slots[slot].text, NOTIFY_TEXT_MAX, "%s", buf);
    g_slots[slot].age_ms = 0;
}

void notify_tick(int dt_ms) {
    for (int i = 0; i < NOTIFY_MAX; i++) {
        if (g_slots[i].age_ms < 0) continue;
        g_slots[i].age_ms += dt_ms;
        if (g_slots[i].age_ms >= NOTIFY_TTL_MS) g_slots[i].age_ms = -1;
    }
}

void notify_render(struct SDL_Renderer *r) {
    if (g_mode != NOTIFY_MODE_SCREEN || !r) return;

    /* Build an ordered list of active entries by age, oldest first
     * (those are drawn highest; newest sits at the bottom). */
    int idx[NOTIFY_MAX], n = 0;
    for (int i = 0; i < NOTIFY_MAX; i++)
        if (g_slots[i].age_ms >= 0) idx[n++] = i;
    if (!n) return;
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (g_slots[idx[i]].age_ms < g_slots[idx[j]].age_ms) {
                int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
            }

    /* Work in the same 1.5x logical space as overlay.c. Restore scale to
     * 1.0 on the way out so we don't disturb the next frame's rendering. */
    int rw, rh;
    SDL_GetRenderOutputSize(r, &rw, &rh);
    int win_h = (int)(rh / NOTIFY_SCALE);
    SDL_SetRenderScale(r, NOTIFY_SCALE, NOTIFY_SCALE);

    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    int y = win_h - BOTTOM_INSET - LINE_H - PAD_Y;
    for (int k = n - 1; k >= 0; k--) {
        NotifyEntry *e = &g_slots[idx[k]];
        int fade_in = NOTIFY_TTL_MS - NOTIFY_FADE_MS;
        Uint8 alpha = 255;
        if (e->age_ms > fade_in) {
            int t = e->age_ms - fade_in;
            int a = 255 - (255 * t) / NOTIFY_FADE_MS;
            if (a < 0) a = 0;
            alpha = (Uint8)a;
        }

        int tw = (int)strlen(e->text) * 8;
        SDL_FRect bg = { (float)MARGIN,
                         (float)y,
                         (float)(tw + 2 * PAD_X),
                         (float)(LINE_H + 2 * PAD_Y) };
        SDL_SetRenderDrawColor(r, 0, 0, 0, (Uint8)((alpha * 180) / 255));
        SDL_RenderFillRect(r, &bg);

        SDL_SetRenderDrawColor(r, 255, 220, 120, alpha);
        SDL_RenderDebugText(r, (float)(MARGIN + PAD_X),
                            (float)(y + PAD_Y + 2), e->text);

        y -= (LINE_H + 2 * PAD_Y + 4);
        if (y < 0) break;
    }

    SDL_SetRenderScale(r, 1.0f, 1.0f);
}

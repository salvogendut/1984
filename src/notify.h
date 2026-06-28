#pragma once

struct SDL_Renderer;

/* Tri-state notification sink for short informational messages (USIfAC
 * PTY/TCP readiness, client connect/disconnect, etc.). Toggle via
 * F9 -> Advanced "Notifications".
 *
 *   NOTIFY_MODE_SCREEN  - fading bottom-left "smoked" toast overlay (default)
 *   NOTIFY_MODE_CONSOLE - stderr only (legacy behaviour)
 *   NOTIFY_MODE_OFF     - silent
 *
 * Single global singleton - call-sites need no context handle. */

typedef enum {
    NOTIFY_MODE_OFF = 0,
    NOTIFY_MODE_SCREEN,
    NOTIFY_MODE_CONSOLE,
} NotifyMode;

void notify_init(void);
void notify_set_mode(NotifyMode mode);

void notify_post(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void notify_tick(int dt_ms);
void notify_render(struct SDL_Renderer *r);

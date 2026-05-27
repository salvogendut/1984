#pragma once
#include "types.h"
#include "cpc.h"
#include <SDL3/SDL.h>
#include <stdbool.h>

typedef struct Monitor Monitor;

Monitor        *monitor_create(CPC *cpc);
void            monitor_destroy(Monitor *mon);
void            monitor_open(Monitor *mon);
bool            monitor_is_open(const Monitor *mon);
/* Returns true if the event was consumed (belongs to the monitor window). */
bool            monitor_handle_event(Monitor *mon, SDL_Event *e);
void            monitor_render(Monitor *mon);
SDL_WindowID    monitor_window_id(const Monitor *mon);

/* Called by main when the emulator hits a breakpoint or completes a step. */
void            monitor_notify_break(Monitor *mon);
void            monitor_notify_step(Monitor *mon);

/* PTY / serial interface (--monitor-pty).
 * monitor_pty_open: opens a PTY master, configures it at 9600 baud (raw),
 *   and returns the slave device path (e.g. "/dev/pts/5") or NULL on error.
 * monitor_pty_tick: call once per frame to drain input and push output. */
const char     *monitor_pty_open(Monitor *mon);
void            monitor_pty_tick(Monitor *mon);

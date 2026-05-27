#pragma once
#include "types.h"
#include "mem.h"
#include <SDL3/SDL.h>
#include <stdbool.h>

typedef struct Monitor Monitor;

Monitor        *monitor_create(Mem *mem);
void            monitor_destroy(Monitor *mon);
void            monitor_open(Monitor *mon);
bool            monitor_is_open(const Monitor *mon);
/* Returns true if the event was consumed (belongs to the monitor window). */
bool            monitor_handle_event(Monitor *mon, SDL_Event *e);
void            monitor_render(Monitor *mon);
SDL_WindowID    monitor_window_id(const Monitor *mon);

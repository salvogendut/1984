/* websvc.h — Web Service: the multi-session "emulator as a service" HTTP
 * server started by --web.
 *
 * Unlike the Web GUI (webgui.h, used by the F9 overlay toggle — one shared
 * CPC mirrored to every browser), each distinct browser (cookie jar) here
 * gets its own, fully isolated CPC instance:
 *
 * GET  /        auto-creates a session on first visit (Set-Cookie), or
 *               re-serves the page for an existing session's cookie
 * GET  /stream  multipart/x-mixed-replace stream of GIF frames
 * POST /key     ?c=<SDL scancode name>&d=1|0   key down/up
 * POST /joy     ?b=0..5&d=1|0                  joystick row-9 bit
 * POST /mouse   ?dx=&dy=&dz=&b=&d=             relative mouse input
 * POST /paste   body = text typed via the paste queue
 * POST /disk    ?drive=0|1&name=<file.dsk>     body = raw .dsk bytes
 * POST /session/config   body = a 1984.conf — rebuilds this session
 * POST /reset   machine reset
 *
 * Every non-"/" endpoint requires a valid session cookie; a missing or
 * stale one gets 400 (a normal browser always hits "/" first).
 *
 * Sessions always boot from config_defaults() — never the host user's
 * real ~/.config/1984/1984.conf. Capped at WEBSVC_MAX_SESSIONS concurrent
 * instances; a session with zero attached streaming clients for longer
 * than WEBSVC_IDLE_TIMEOUT_MS is destroyed to free the slot. Binds
 * 0.0.0.0 with no authentication, same LAN-trust model as the Web GUI —
 * session isolation means a new browser gets a new *machine*, not new
 * *credentials*.
 *
 * Self-contained: owns its own SDL_Init, listening socket, and 50 Hz
 * pacing loop. Does not touch webgui.c/webgui.h (the overlay-toggle path)
 * at all.
 */
#pragma once

/* Runs until Ctrl-C/SIGTERM. Returns a process exit code. */
int websvc_run(int port);

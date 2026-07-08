/* webgui.h — embedded Web GUI: an HTTP server that serves the emulator
 * screen and controls to a browser on the LAN.
 *
 * GET  /        self-contained HTML page (video + keyboard/joystick/paste)
 * GET  /stream  multipart/x-mixed-replace stream of GIF frames
 * POST /key     ?c=<SDL scancode name>&d=1|0   key down/up
 * POST /joy     ?b=0..5&d=1|0                  joystick row-9 bit
 * POST /paste   body = text typed via the paste queue
 * POST /reset   machine reset
 *
 * Single-threaded and non-blocking: webgui_poll() runs once per frame
 * before cpc_frame() (input lands on the next emulated frame),
 * webgui_frame() once per frame after it (encodes and pushes video).
 * Binds 0.0.0.0 with no authentication — LAN-trust only.
 */
#pragma once
#include <stdbool.h>
#include "cpc.h"
#include "paste.h"

/* Capture machine pointers once; opens no socket. */
void webgui_init(CPC *cpc, Paste *paste);

/* Log server lifecycle and client activity to stderr (listen URLs,
 * viewer connect/disconnect). Enabled by --web for journald/log
 * capture in headless service use; off by default (toasts only). */
void webgui_set_log(bool on);

/* Start listening on 0.0.0.0:port. Posts a notify toast either way;
 * returns false if the socket could not be opened. */
bool webgui_start(int port);

/* Close the listener and all clients, release any web-held joystick
 * bits. Safe to call when not running. */
void webgui_stop(void);

bool webgui_active(void);
int  webgui_port(void);       /* port currently listening on, 0 if inactive */

void webgui_poll(void);       /* per frame BEFORE cpc_frame */
void webgui_frame(void);      /* per frame AFTER cpc_frame */

#pragma once
#include <stdbool.h>
#include "cpc.h"
#include "display.h"
#include "paste.h"

/*
 * pilot — an "auto-pilot" input device driven from a host PTY (/dev/pts).
 *
 * Opened with --pilot[=LINK]. 1984 prints (and notifies) the slave path; an
 * external driver — a script, or an AI given full control of the guest
 * desktop during debugging — writes newline-delimited polar-coordinate
 * commands to it. The pilot translates them into mouse motion / button
 * presses (SymbiFace II + Albireo/CH376) or CPC joystick 1 (matrix row 9).
 *
 * It's the interactive cousin of --joy-script: where --joy-script replays a
 * fixed DIRS:FRAMES timeline, the pilot reacts live to whatever the host
 * sends, and uses a held-velocity model so a single command keeps the
 * pointer gliding until changed.
 *
 * Protocol (one command per line; case-insensitive; tokens split on spaces
 * or commas; '#' starts a comment):
 *
 *   move R THETA   set velocity vector, held until changed. R = magnitude,
 *   <R> <THETA>    THETA = angle in degrees (0=right, 90=up, CCW positive).
 *   v R THETA      A bare "R THETA" line (starts with a digit) is the same.
 *                  Mouse target: R is pixels/frame (50 Hz), so R=5 ≈ 250 px/s;
 *                  sub-pixel speeds accumulate. Joystick target: THETA snaps
 *                  to the nearest 8-way direction and R>0 engages it.
 *   stop           R=0 (also: s, halt, x).
 *   press N        press button N (1=left/Fire1, 2=right/Fire2, 3=middle). p.
 *   release N      release button N (also: u).
 *   click N        press button N and auto-release after a few frames (c).
 *   hold F R THETA hold the vector for F frames, then auto-stop.
 *   hold-click F N press button N for F frames, then auto-release.
 *   scroll DZ      mouse wheel by DZ (mouse target only).
 *   key-down NAME   press a CPC key by SDL scancode name (e.g. A, Return).
 *   key-up NAME     release that key.
 *   key-tap NAME F  press for F frames, then release.
 *   paste TEXT      queue TEXT through the existing paste path.
 *   target mouse   route to the mouse pointer (default). Aliases: t m.
 *   target joy     route to CPC joystick 1 (row 9). Aliases: t j / joystick.
 *   reset          stop and release every button/direction.
 *   state          emit a machine-readable state line on the PTY.
 *   frame          emit the current frame counter.
 *   hash           emit the current framebuffer hash.
 *   changed        emit the latest changed-rectangle summary.
 *   wait ...       block until a frame/hash/change/quiet condition is met.
 *   crop PATH X Y W H [SCALE]  save a cropped screenshot.
 *   snapshot-save PATH / snapshot-load PATH
 *
 * Linux/POSIX only (PTYs); Windows builds get no-op stubs.
 */

typedef enum { PILOT_MOUSE = 0, PILOT_JOY = 1 } PilotTarget;
typedef enum {
    PILOT_WAIT_NONE = 0,
    PILOT_WAIT_FRAMES,
    PILOT_WAIT_HASH_EQ,
    PILOT_WAIT_HASH_NE,
    PILOT_WAIT_CHANGE,
    PILOT_WAIT_QUIET,
} PilotWaitMode;

typedef struct {
    int          fd;            /* master PTY fd, -1 when closed         */
    char         slave[256];    /* /dev/pts/N slave path                 */
    char         link[256];     /* optional stable symlink alias         */
    bool         reply_stderr;  /* mirror machine replies to stderr      */
    PilotTarget  target;

    double       mag, ang;      /* last commanded polar vector           */
    double       vx, vy;        /* derived per-frame velocity (screen y+ = down) */
    double       acc_x, acc_y;  /* sub-pixel motion remainder            */

    bool         btn[3];        /* current button state (0=L 1=R 2=M)    */
    int          click_left[3]; /* frames until auto-release (0 = none)  */
    int          move_left;     /* frames until auto-stop (0 = none)      */
    int          keytap_left;   /* frames until auto-release of key tap   */
    int          keytap_scancode;
    int          scroll_pending;/* wheel delta queued for the next tick  */
    bool         joy_held;      /* row 9 is currently being driven       */

    int          frame_count;   /* most recent completed frame           */
    u32          fb_hash;       /* most recent completed framebuffer hash */
    u32         *last_pixels;   /* previous frame for diffing             */
    bool         have_last_pixels;
    int          changed_x, changed_y, changed_w, changed_h;
    int          quiet_frames;

    PilotWaitMode wait_mode;
    int           wait_target_frames;
    int           wait_start_frame;
    int           wait_timeout_frame;   /* -1 = no timeout */
    u32           wait_hash;
    int           wait_quiet_need;

    char         line[256];     /* partial-line accumulator              */
    int          line_len;
} Pilot;

/* Open the pilot PTY. link may be NULL/empty (no symlink). initial sets the
 * starting target. reply_stderr mirrors machine replies to stderr for
 * harnesses that want one-shot PTY writes plus out-of-band responses.
 * Returns true on success; on failure leaves fd = -1. */
bool pilot_open(Pilot *p, const char *link, PilotTarget initial, bool reply_stderr);

bool pilot_is_open(const Pilot *p);

/* Pre-frame pass: drain pending commands and apply input state for the next
 * emulated frame. */
void pilot_tick(Pilot *p, CPC *cpc, Display *d, Paste *paste);

/* Post-frame pass: refresh diagnostics state from the completed frame and
 * complete any pending waits. frame_count is the just-finished frame. */
void pilot_post_frame(Pilot *p, CPC *cpc, Display *d, int frame_count);

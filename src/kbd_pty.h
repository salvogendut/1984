/* kbd_pty — POSIX pseudo-terminal that injects characters into the CPC
 * keyboard matrix. Lets external scripts drive BASIC / CP/M+ / SymbOS
 * programmatically without going through xdotool + SDL. Pairs naturally
 * with the existing --monitor-pty for debugger-side access. Used by the
 * issue #129 investigation harness; off by default and gated on
 * --kbd-pty so the host CPU isn't penalised when nobody connects. */
#pragma once
#include <stdbool.h>
#include "paste.h"

/* Open the PTY. Returns the slave device path (e.g. /dev/pts/12) on
 * success, NULL on failure. Caller doesn't free the returned pointer
 * — it's owned by an internal static buffer. */
const char *kbd_pty_open(void);

/* Drain available characters from the PTY and feed them to the existing
 * paste machinery so they get typed into the keyboard matrix. Call once
 * per frame, before cpc_frame(). No-op if not opened. */
void kbd_pty_tick(Paste *p);

/* True if a PTY was opened successfully. */
bool kbd_pty_is_open(void);

/* Stream a single character out the PTY. Called from cpc.c when the Z80
 * enters the firmware TXT WR CHAR vector (&BB5A) so external readers
 * see the CPC's text output in real time. */
void kbd_pty_emit_char(unsigned char c);

/* Stream a buffer out the PTY in one write() call — much more efficient
 * than emit_char for bulk output (~1 KB screen-text frames every frame
 * would otherwise saturate the master-side buffer in single-byte syscalls
 * and starve the input-read path). Best-effort; partial writes are dropped. */
void kbd_pty_emit_buf(const void *buf, int len);

/* usifac — USIfAC II RS232 serial interface emulation.
 *
 * The USIfAC II (John Konstantopoulos) is a PIC18F47Q10-based serial
 * board for the Amstrad CPC. We emulate the wire-level RS232 surface
 * only — the host-side ROM (RSX commands, USB host, FDC/ROM
 * emulation) is out of scope.
 *
 * Ports (CPC I/O, all gated on the MX4 expansion bus):
 *   &FBD0  r/w  DATA      — pop/push one byte from the RX/TX queue
 *   &FBD1  r    STATUS    — 0xFF when RX non-empty; when empty returns
 *                            the "empty sentinel" (default 0x01; flipped
 *                            to 0x00 by control cmd 40, back by cmd 41)
 *   &FBD1  w    CONTROL   — see usifac_write() comments for the command map
 *   &FBD8  r    EXISTS    — real firmware returns the board's rom_number
 *                            (0..6) so the companion host-ROM can call
 *                            back into itself after a bank switch; 0xFF
 *                            means absent. We don't ship the host-ROM
 *                            and only emulate the serial pipe, so we
 *                            return 0x00 — passes the "not 0xFF"
 *                            presence check used by every pure-serial
 *                            consumer (FUZIX, Hayes/PerryFi, raw I/O),
 *                            and steers any hypothetical ROM-aware code
 *                            away from a bogus bank switch (slot 0 is
 *                            the lower OS ROM, never an expansion).
 *   &FBDD  r    BAUDCODE  — last x written to &FBD1 in range 10..23
 *
 * Backend: a host-side PTY or a TCP listener, selected via cfg.
 * Polled once per emulated frame; drains backend into RX, pushes
 * TX out to backend.
 */
#pragma once

#include "types.h"
#include <stdbool.h>
#include <stddef.h>

struct Perryfi;

#define USIFAC_RING (4096)  /* power of two; >= 2900 (firmware RX buffer) */

typedef enum {
    USIFAC_BACKEND_PTY = 0,
    USIFAC_BACKEND_TCP = 1,
} UsifacBackend;

typedef struct {
    bool present;             /* mirror of cfg->usifac at init time */
    UsifacBackend backend;

    /* PTY backend */
    int  pty_master;          /* -1 when closed */
    char pty_slave[64];       /* /dev/pts/N for the overlay */
    char pty_link[512];       /* stable alias symlinked to pty_slave; "" when disabled */

    /* TCP backend */
    int  tcp_listen;          /* -1 when closed */
    int  tcp_client;          /* -1 when no client */
    int  tcp_port;

    /* Ring buffers — power-of-two sized, masked indices */
    u8     rx_buf[USIFAC_RING];
    size_t rx_head, rx_tail;  /* head=push (backend), tail=pop (Z80) */
    u8     tx_buf[USIFAC_RING];
    size_t tx_head, tx_tail;  /* head=push (Z80),     tail=pop (backend) */

    bool burst_mode;
    u8   baud_code;           /* last 10..23 command via OUT &FBD1,x */
    u8   empty_sentinel;      /* value returned from &FBD1 read when RX
                               * empty; default 0x01, cmds 40/41 flip it */

    /* When non-NULL AND p->present, the data port (&FBD0) routes
     * through the AT-modem instead of touching the PTY/TCP rings.
     * The control bytes (&FBD1 baud codes / burst mode), status
     * (&FBD1 read), and presence (&FBD8) reads still flow through
     * USIfAC unchanged — only data is hijacked. */
    struct Perryfi *perryfi;
} USIfAC;

/* `backend` must be "pty" or "tcp"; `tcp_port` is ignored for PTY.
 * `pty_link_path` (PTY backend only) is an optional host-side path to
 * symlink at the live /dev/pts/N slave so external tools can find it
 * at a stable location across launches. NULL or "" disables the alias. */
void    usifac_init    (USIfAC *u, bool enable, const char *backend, int tcp_port,
                        const char *pty_link_path);
void    usifac_shutdown(USIfAC *u);

/* Plug the AT-modem in. Call after init; passing NULL detaches.
 * Routing decision is taken per-byte based on perryfi->present, so
 * toggling perryfi at runtime is safe with no further usifac calls. */
void    usifac_attach_perryfi(USIfAC *u, struct Perryfi *p);

/* CPC bus interface. `lo` is the low byte of the I/O port (D0/D1/D8/DD/...). */
u8   usifac_read (USIfAC *u, u8 lo);
void usifac_write(USIfAC *u, u8 lo, u8 val);

/* Drain backend → RX, push TX → backend. Call once per frame from cpc.c. */
void usifac_poll(USIfAC *u);

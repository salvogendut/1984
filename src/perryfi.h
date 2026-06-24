/* perryfi — software AT-modem that bridges the CPC USIfAC II serial
 * port to the host's TCP/IP stack.
 *
 * The real PerryFi (by SanPollo) is a Wemos D1 mini (ESP8266) board
 * that plugs onto a CPC serial interface and exposes a Hayes AT
 * command set; CP/M terminal programs see it as a 9600 bps modem and
 * use ATDT to dial Telnet/Telephony hosts over WiFi.
 *
 * 1984 doesn't model the radio — when this extension is enabled the
 * AT command interpreter lives inside the emulator and forwards
 * ATDT host:port to a host-side TCP socket. WiFi-config commands
 * (AT$SSID=, AT$PASS=, AT$MDNS=) are parsed and accepted as no-ops.
 *
 * Wiring: when ext_perryfi is on, the USIfAC data port (&FBD0)
 * reads/writes route through Perryfi instead of the raw pty/tcp
 * USIfAC backend.
 *
 * Ported from 1985/src/perryfi.{c,h} — see that file for the
 * reference shape. Differences here are limited to the AT&I
 * identification banner and the doc references.
 */
#pragma once

#include "types.h"
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>

#define PERRYFI_RING       4096   /* power of two */
#define PERRYFI_CMD_MAX     256
#define PERRYFI_ESC_CHAR    '+'
#define PERRYFI_ESC_COUNT     3

typedef enum {
    PERRYFI_STATE_CMD     = 0,    /* AT-command mode */
    PERRYFI_STATE_ONLINE  = 1,    /* TCP-bridge mode */
} PerryfiState;

typedef struct Perryfi {
    bool present;                 /* mirror of cfg->perryfi at init time */

    PerryfiState state;

    /* AT line buffer (host → modem) */
    char  cmd_buf[PERRYFI_CMD_MAX + 1];
    int   cmd_len;
    char  last_cmd[PERRYFI_CMD_MAX + 1];   /* A/ repeat */

    /* Modem-side settings — kept in RAM only (no AT&W persistence). */
    bool  echo;
    bool  quiet;
    bool  verbose;
    bool  extended_codes;

    /* TCP client */
    int   tcp_fd;                 /* -1 when disconnected */
    char  remote_host[128];
    int   remote_port;

    /* +++ escape detection (only meaningful in ONLINE state) */
    int   esc_count;
    Uint64 esc_last_ms;

    /* Modem → guest queue (result strings, TCP bytes). */
    u8     rx_buf[PERRYFI_RING];
    size_t rx_head, rx_tail;
} Perryfi;

void perryfi_init    (Perryfi *p, bool enable);
void perryfi_shutdown(Perryfi *p);

/* Called once per frame: pump the +++ escape timer and drain the TCP
 * socket into the modem→guest RX queue. */
void perryfi_poll(Perryfi *p);

/* Guest-facing byte API (matches USIfAC's data-port semantics). */
bool perryfi_rx_pop (Perryfi *p, u8 *out);
bool perryfi_tx_push(Perryfi *p, u8 b);
bool perryfi_rx_has (const Perryfi *p);

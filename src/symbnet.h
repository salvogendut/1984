/* symbnet.h — 1984 emulator's synthetic SymbOS network port.
 *
 * Talks to the SymbOS netd-1984.exe driver via two CPC I/O ports:
 *   0xFD30  R/W  data FIFO (command bytes in, response bytes out)
 *   0xFD31   R   status (bit 0 = response ready, bit 1 = last cmd errored)
 *
 * Wire format and opcodes are documented in
 *   ../../symsys-networkdaemon-1984/README.md
 */
#pragma once
#include "types.h"
#include <stdbool.h>

#define SYMBNET_NSOCKS 16
#define SYMBNET_BUFLEN 4096

typedef struct {
    int  fd;                /* host POSIX fd, -1 = unused */
    u8   state;             /* 0=unused, 1=listen, 2=estab, 3=closewait, 4=closed */
    bool is_udp;
    u16  local_port;
    u8   remote_ip[4];
    u16  remote_port;
} SymbNetSock;

typedef struct {
    /* Command FIFO (CPU → host) */
    u8   cmd[SYMBNET_BUFLEN];
    int  cmd_len;           /* bytes received so far */
    int  cmd_expected;      /* total bytes expected (after length header decoded) */

    /* Response FIFO (host → CPU) */
    u8   resp[SYMBNET_BUFLEN];
    int  resp_len;          /* total bytes ready to read */
    int  resp_pos;          /* next byte to return on FD30 read */

    bool last_error;        /* mirrored to FD31 bit 1 */

    SymbNetSock sockets[SYMBNET_NSOCKS];
} SymbNet;

void symbnet_init(SymbNet *n);
void symbnet_reset(SymbNet *n);

/* Per-frame poll: accepts incoming server connections, no other side effects. */
void symbnet_tick(SymbNet *n);

/* Port handlers. port_lo is 0x30 (data) or 0x31 (status). */
u8   symbnet_port_read(SymbNet *n, u8 port_lo);
void symbnet_port_write(SymbNet *n, u8 port_lo, u8 val);

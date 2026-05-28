/* symbnet.h — 1984 emulator's synthetic SymbOS network port.
 *
 * Re-exposes the M4 board's command dispatcher over a single FIFO pair
 * so a stripped-down SymbOS network daemon (netd-1984.exe) can drive it
 * without any ROM scanning, bus-mapped buffers, or bank tricks.
 *
 *   0xFD30  R/W  data FIFO (M4 command bytes in, response bytes out)
 *   0xFD31   R   status (bit 0 = response ready, bit 1 = last cmd errored)
 *           W    strobe — ignored (the FIFO auto-executes when the
 *                length-prefixed packet is complete)
 *
 * Wire format is the M4 packet format byte-for-byte:
 *   first byte = N (count of bytes that follow), then opcode-lo,
 *   opcode-hi, args. Opcodes are the standard M4_C* values.
 *
 * Protocol details and the daemon glue live in
 *   ../../symsys-networkdaemon-1984/README.md
 */
#pragma once
#include "types.h"
#include "m4.h"
#include <stdbool.h>

typedef struct {
    M4  *m4;                /* upstream M4 instance providing the dispatcher */

    /* Incoming command bytes from FD30 writes. First byte is the count of
     * bytes that follow; when (cmd_len == 1 + cmd[0]) we hand them off to
     * the M4 dispatcher. */
    u8   cmd[M4_CMD_BUF];
    int  cmd_len;

    /* The dispatcher writes its response to m4->bus_mem; we pull bytes out
     * of there byte-by-byte as the daemon reads from FD30. */
    int  resp_pos;
    bool resp_ready;
    bool last_error;
} SymbNet;

void symbnet_init(SymbNet *n, M4 *m4);
void symbnet_reset(SymbNet *n);
void symbnet_tick(SymbNet *n);

u8   symbnet_port_read(SymbNet *n, u8 port_lo);
void symbnet_port_write(SymbNet *n, u8 port_lo, u8 val);

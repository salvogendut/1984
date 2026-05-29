/* net4cpc.h — W5100S / Net4CPC hardware emulation
 *
 * The Net4CPC add-on board exposes four Z80 I/O ports:
 *   0xFD20  MR      – Mode Register (reads 0x03 when chip present)
 *   0xFD21  IDM_ARH – high byte of 16-bit indirect address
 *   0xFD22  IDM_ARL – low byte  of 16-bit indirect address
 *   0xFD23  IDM_DR  – data read/write; address auto-increments after each
 *                     access when MR bit 1 (AI) is set
 *
 * Socket operations are backed by host POSIX sockets.
 */
#pragma once
#include "types.h"

extern int net4cpc_trace;            /* set by --trace-net4cpc */

void net4cpc_reset(void);
u8   net4cpc_in(u8 reg_sel);         /* reg_sel = port_low & 0x03 */
void net4cpc_out(u8 reg_sel, u8 val);

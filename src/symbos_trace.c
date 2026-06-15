/* SymbOS message-bus tracing.
 *
 * Hooks Z80 RST #10 (the SymbOS message-send vector). At RST #10 entry the
 * SymbiosiS netmsg1 calling convention has IY pointing at App_MsgBuf, IX
 * holding (target_pid << 8) | source_pid, and App_MsgBuf+0 holding the
 * message type byte. We log net-daemon-class messages so we can see
 * whether netd-m4c.exe ever emits MSR_NET_TCPEVT (=159) during the
 * settime-hang scenario.
 *
 * This module observes only — it never alters CPU state. Active solely
 * when --trace-symbos-msg is given on the command line. */

#include "symbos_trace.h"
#include "z80.h"
#include <stdbool.h>
#include <stdio.h>

/* SymbOS message type ranges (from SymbOS-Constants.asm in the
 * ASM-Developer-kit). Daemon→app messages are MSR_NET_* in 128..191
 * (TCPEVT=159, UDPEVT=175). App→daemon function calls are FNC_NET_*
 * in the function-call range (e.g. TCPOPN=16, TCPSND=20). We want
 * both directions when diagnosing settime's TCP wedge. */
extern int cpc_frame_count;   /* defined in cpc.c */

static void hook_rst10(Z80 *cpu, Z80Bus *bus) {
    /* Read message-type byte from App_MsgBuf via the bus so banking
     * is respected — App_MsgBuf lives in the caller process's bank. */
    u8 msg = bus->mem_read(bus->ctx, cpu->iy);
    u8 src = (u8)(cpu->ix & 0xFF);
    u8 dst = (u8)(cpu->ix >> 8);

    /* Filter: only messages that touch the net daemon (typically proc id
     * 0x07) OR fall in the MSR_NET_* range. Avoids flooding the trace
     * with unrelated kernel/desktop messaging. */
    bool is_net_msr = (msg >= 128 && msg <= 191);
    bool touches_netd = (src == 0x07 || dst == 0x07);
    if (!is_net_msr && !touches_netd) return;

    fprintf(stderr,
            "[symbos f%d] RST10 msg=%02X src=%02X dst=%02X IY=%04X PC=%04X buf:",
            cpc_frame_count, msg, src, dst, cpu->iy, cpu->pc);
    for (int i = 0; i < 16; i++)
        fprintf(stderr, " %02X", bus->mem_read(bus->ctx, (u16)(cpu->iy + i)));
    fprintf(stderr, "\n");
}

void symbos_trace_enable(void) {
    z80_rst10_hook = hook_rst10;
}

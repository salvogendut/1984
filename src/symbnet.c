#include "symbnet.h"
#include "m4.h"
#include <string.h>

/* Run a buffered M4 packet through the dispatcher and queue the response
 * for FD30 reads. */
static void execute(SymbNet *n) {
    /* Copy our captured packet into the M4's command buffer and hand off to
     * the existing dispatcher — same code path that real M4 ack-port writes
     * take. mem is unused by the dispatcher (its only side-effect is writing
     * the response into m4->bus_mem). */
    int len = n->cmd_len;
    if (len > M4_CMD_BUF) len = M4_CMD_BUF;
    memcpy(n->m4->cmd_buf, n->cmd, (size_t)len);
    n->m4->cmd_len = len;
    (void)m4_ackport_write(n->m4, NULL);

    /* M4 dispatcher writes error code as bus_mem[0]. */
    n->last_error = (n->m4->bus_mem[0] != 0);
    n->resp_pos   = 0;
    n->resp_ready = true;
}

void symbnet_init(SymbNet *n, M4 *m4) {
    memset(n, 0, sizeof(*n));
    n->m4 = m4;
}

void symbnet_reset(SymbNet *n) {
    n->cmd_len   = 0;
    n->resp_pos  = 0;
    n->resp_ready = false;
    n->last_error = false;
}

void symbnet_tick(SymbNet *n) {
    /* Keep sock_info fresh so the daemon's status reads see in-flight
     * connect completion and pending RX bytes between commands. */
    if (n->m4) m4_tick(n->m4);
}

u8 symbnet_port_read(SymbNet *n, u8 port_lo) {
    if (port_lo == 0x30) {
        if (n->resp_ready && n->resp_pos < (int)sizeof(n->m4->bus_mem))
            return n->m4->bus_mem[n->resp_pos++];
        return 0;
    }
    if (port_lo == 0x31) {
        u8 s = 0;
        if (n->resp_ready)   s |= 0x01;
        if (n->last_error)   s |= 0x02;
        return s;
    }
    return 0xFF;
}

void symbnet_port_write(SymbNet *n, u8 port_lo, u8 val) {
    if (port_lo == 0x31) {
        /* Strobe — no-op. Packets auto-execute when complete. */
        return;
    }
    if (port_lo != 0x30) return;

    /* New packet starting: drop the previous response. */
    if (n->cmd_len == 0) {
        n->resp_ready = false;
        n->resp_pos = 0;
    }

    if (n->cmd_len < (int)sizeof(n->cmd))
        n->cmd[n->cmd_len++] = val;

    /* M4 wire format: first byte = count of bytes that follow. Trigger
     * execution once we have all of them. */
    if (n->cmd_len >= 1) {
        int expected = 1 + n->cmd[0];
        if (n->cmd_len >= expected) {
            execute(n);
            n->cmd_len = 0;
        }
    }
}

#pragma once

/* Enable RST #10 tracing for SymbOS message sends.
 * Wires up z80_rst10_hook to log every net-daemon-class message that
 * passes through SymbOS's message-dispatch primitive. See plan/woolly
 * for context — used to determine whether netd-m4c.exe ever emits
 * MSR_NET_TCPEVT during the settime hang scenario. */
void symbos_trace_enable(void);

/* Browser transport for the M4 board's network stack.
 *
 * The native M4 dials host POSIX sockets directly. A browser sandbox cannot
 * open TCP sockets, so under __EMSCRIPTEN__ the socket operations in src/m4.c
 * delegate to the functions below, which talk to a JS bridge (m4-bridge.js)
 * that tunnels DNS/TCP through a restricted WebSocket relay (relay/server.js).
 *
 * The API is deliberately poll-driven: src/m4.c already polls socket state
 * every frame (net_poll_socket / m4_tick), so async relay completions are
 * simply observed on the next poll instead of re-entering wasm.
 */
#ifndef M4_WEB_H
#define M4_WEB_H

#include <stddef.h>
#include <stdint.h>

/* Bit flags returned by m4_web_poll(). */
#define M4_WEB_POLL_CONNECTED 0x01u  /* in-flight connect finished OK */
#define M4_WEB_POLL_FAILED    0x02u  /* in-flight connect failed */
#define M4_WEB_POLL_CLOSED    0x04u  /* remote closed / connection lost */
#define M4_WEB_POLL_RX        0x08u  /* buffered RX bytes available */

/* Dial a TCP connection to ip:port from the given M4 socket (channel).
 * Returns 1 when the dial was queued (connecting), 0 if already established,
 * or -1 on an immediate failure (relay offline, invalid channel). */
int m4_web_connect(int channel, const uint8_t ip[4], uint16_t port);

/* Poll a channel. Returns a M4_WEB_POLL_* bitmask (0 = idle/open, no news). */
int m4_web_poll(int channel);

/* Number of RX bytes buffered for the channel, or -1 when unknown. */
int m4_web_avail(int channel);

/* Send data on an established channel. Returns 0 on success, -1 on error. */
int m4_web_send(int channel, const uint8_t *data, size_t length);

/* Drain up to maxlen buffered RX bytes into data. Returns the byte count, 0
 * when nothing is buffered yet, or -1 when the channel is gone. */
int m4_web_recv(int channel, uint8_t *data, size_t maxlen);

/* Close a channel (idempotent). */
void m4_web_close(int channel);

/* Start an asynchronous DNS lookup. Returns 1 when queued, 0 on error. */
int m4_web_dns(const char *host);

/* Poll the pending DNS lookup. Returns 0 while pending, 1 on success (writes
 * the IPv4 address to ip_out), or -1 on failure. */
int m4_web_dns_poll(uint8_t ip_out[4]);

#endif /* M4_WEB_H */

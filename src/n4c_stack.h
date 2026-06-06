/* n4c_stack.h — TCP/IP/UDP/ICMP/ARP stack on top of the TAP backend.
 *
 * Layered above tap.c and below the W5100S register emulation in
 * net4cpc.c. When a TAP fd is attached, the W5100S socket commands
 * route their TX/RX through this stack instead of through POSIX
 * sockets, so the board behaves as a real L2 endpoint on the host's
 * bridge.
 *
 * Public surface (all functions are no-ops when no TAP fd has been
 * attached via n4c_stack_attach()):
 *   n4c_stack_attach()  — bind to a TAP fd + tell us our MAC/IP/GW/mask
 *   n4c_stack_poll()    — drain incoming Ethernet frames; dispatches
 *                         ARP/IP per type, fires ICMP echo replies,
 *                         queues received UDP/TCP to the W5100S sockets
 *   n4c_stack_arp_resolve() — async ARP lookup; returns true and fills
 *                         out_mac if already cached, else fires a
 *                         request and returns false
 *   n4c_stack_send_udp() — assemble Ethernet+IP+UDP and write to TAP
 *
 * Big-endian on the wire; everything else native order.
 */
#pragma once
#include "types.h"
#include <stdbool.h>

/* Attach the stack to a TAP fd and tell it the host-configured
 * SHAR/SIPR/GAR/SUBR. Safe to re-attach with new values whenever the
 * W5100S kernel writes new common registers. fd == -1 detaches. */
void n4c_stack_attach(int tap_fd,
                      const u8 mac[6],
                      const u8 sipr[4],
                      const u8 gar[4],
                      const u8 subr[4]);

/* Tell us about a new SHAR/SIPR/GAR/SUBR without changing the TAP fd.
 * Called by net4cpc.c whenever the kernel writes those registers so
 * the stack reflects the live config without bouncing the TAP. */
void n4c_stack_update_config(const u8 mac[6],
                             const u8 sipr[4],
                             const u8 gar[4],
                             const u8 subr[4]);

/* Once per emulated frame: drain inbound Ethernet, dispatch. */
void n4c_stack_poll(void);

/* True if the stack has a TAP attached and a valid SIPR. */
bool n4c_stack_active(void);

/* Look up the MAC for an IPv4 address in the local subnet (or the
 * gateway's MAC if dst is off-subnet). Returns true if the cache
 * already has the entry (out_mac is filled). If not cached, fires an
 * ARP request and returns false; the caller should retry later. */
bool n4c_stack_arp_resolve(const u8 dst_ip[4], u8 out_mac[6]);

/* Send a UDP datagram with the given source port (the W5100S socket's
 * Sn_PORT). Returns the number of payload bytes that were written, or
 * -1 if the next-hop MAC isn't known yet (in which case the stack has
 * already queued an ARP query; the caller should retry next frame). */
int  n4c_stack_send_udp(u16 src_port,
                        const u8 dst_ip[4], u16 dst_port,
                        const u8 *payload, u16 payload_len);

/* Hook installed by net4cpc.c: called by the stack when a UDP datagram
 * arrives that's bound for one of our local ports. The W5100S
 * formats received UDP with an 8-byte header (src IP, src port, len).
 * Returning 0 = drop (no matching socket), non-zero = accepted. */
typedef int (*N4CUdpDeliver)(const u8 src_ip[4], u16 src_port,
                             u16 dst_port,
                             const u8 *payload, u16 payload_len);
void n4c_stack_set_udp_deliver(N4CUdpDeliver fn);

/* -- TCP ----------------------------------------------------------------
 * Per-socket TCP control blocks are owned by the stack; the W5100S
 * register emulation in net4cpc.c calls the n4c_stack_tcp_* helpers
 * when it sees the matching SCMD_*, and the stack calls back into
 * net4cpc.c through three function pointers to mutate the W5100S
 * register state. */

void n4c_stack_tcp_open(int sock, u16 local_port);
void n4c_stack_tcp_listen(int sock);

/* Returns true if the SYN was sent (handshake started); false if ARP
 * resolution is pending (caller will be retriggered when the kernel
 * polls again). */
bool n4c_stack_tcp_connect(int sock, const u8 dst_ip[4], u16 dst_port);

/* Hand bytes to the stack; the stack will transmit them respecting
 * MSS, hold the unacked bytes for retransmit, and call the ack-cb
 * with the number that the peer ACKs. Returns bytes accepted (always
 * == len; the kernel's TX FSR already gated this). */
int  n4c_stack_tcp_send(int sock, const u8 *data, int len);

/* Graceful close (sends FIN). */
void n4c_stack_tcp_disconnect(int sock);

/* Hard close (sends RST then discards state). */
void n4c_stack_tcp_close(int sock);

/* Callbacks back into net4cpc.c. on_state is invoked whenever the
 * W5100S socket should transition state (new_sr is the W5100S Sn_SR
 * value); it should set Sn_SR and raise the right Sn_IR bits (CON on
 * ESTABLISHED, DISCON on CLOSED, TIMEOUT on a 4-retry RTO blow-up).
 * on_data is invoked for inbound bytes; it should push them into the
 * RX ring and raise RECV. on_ack is invoked when the peer ACKs
 * outbound bytes; it should advance TX_RD by `acked` and raise
 * SEND_OK. */
typedef void (*N4CTcpEvent)(int sock, u8 new_sr);
typedef int  (*N4CTcpDeliver)(int sock, const u8 *data, int len);
typedef void (*N4CTcpAck)(int sock, u16 acked);
void n4c_stack_set_tcp_callbacks(N4CTcpEvent on_state,
                                 N4CTcpDeliver on_data,
                                 N4CTcpAck on_ack);

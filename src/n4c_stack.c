/* n4c_stack.c — see n4c_stack.h
 *
 * This compilation unit owns: the ARP cache, the IPv4 header build/parse,
 * the ICMP echo responder, and the UDP send/route surface. TCP lives in
 * the same file (phase 6 of #104) but is added in a later commit.
 */
#include "n4c_stack.h"
#include "tap.h"
#include "leds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int n4c_stack_trace = 0;

#define TLOG(...) do { if (n4c_stack_trace) fprintf(stderr, "[n4c] " __VA_ARGS__); } while (0)

/* ---------------------------------------------------------------------------
 * Live config
 * ------------------------------------------------------------------------- */

static int  s_tap_fd = -1;
static u8   s_mac [6];
static u8   s_sipr[4];
static u8   s_gar [4];
static u8   s_subr[4];

static N4CUdpDeliver   s_udp_deliver    = NULL;
static N4CRawIpDeliver s_raw_ip_deliver = NULL;
static N4CTcpEvent     s_tcp_state      = NULL;
static N4CTcpDeliver   s_tcp_data       = NULL;
static N4CTcpAck       s_tcp_ack        = NULL;

void n4c_stack_set_udp_deliver(N4CUdpDeliver fn)   { s_udp_deliver    = fn; }
void n4c_stack_set_ip_deliver (N4CRawIpDeliver fn) { s_raw_ip_deliver = fn; }

void n4c_stack_set_tcp_callbacks(N4CTcpEvent on_state,
                                 N4CTcpDeliver on_data,
                                 N4CTcpAck on_ack) {
    s_tcp_state = on_state;
    s_tcp_data  = on_data;
    s_tcp_ack   = on_ack;
}

bool n4c_stack_active(void) {
    /* As soon as a TAP is bound we own the wire — DHCP-style flows with
     * SIPR=0.0.0.0 are legitimate and the only path that reaches a
     * tap-bound DHCP server. The earlier "wait for non-zero SIPR" gate
     * routed those through the host POSIX socket instead, which is the
     * wrong interface entirely when the host's DHCP responder is bound
     * to tap0. */
    return s_tap_fd >= 0;
}

void n4c_stack_update_config(const u8 mac[6],
                             const u8 sipr[4],
                             const u8 gar[4],
                             const u8 subr[4]) {
    memcpy(s_mac,  mac,  6);
    memcpy(s_sipr, sipr, 4);
    memcpy(s_gar,  gar,  4);
    memcpy(s_subr, subr, 4);
}

void n4c_stack_attach(int tap_fd,
                      const u8 mac[6],
                      const u8 sipr[4],
                      const u8 gar[4],
                      const u8 subr[4]) {
    s_tap_fd = tap_fd;
    n4c_stack_update_config(mac, sipr, gar, subr);
}

/* ---------------------------------------------------------------------------
 * ARP cache — small fixed table with LRU eviction. 60-second TTL roughly
 * matches typical OS behaviour and is well within ARP's RFC-recommended
 * 20-minute upper bound for an emulator that isn't tracking traffic age.
 * ------------------------------------------------------------------------- */

#define ARP_CACHE_SIZE 16
#define ARP_TTL_SEC    60

typedef struct {
    u8     ip[4];
    u8     mac[6];
    time_t ts;       /* monotonic-ish: time(NULL) is fine here */
    bool   valid;
} ArpEntry;

static ArpEntry s_arp[ARP_CACHE_SIZE];

static ArpEntry *arp_find(const u8 ip[4]) {
    time_t now = time(NULL);
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        ArpEntry *e = &s_arp[i];
        if (!e->valid) continue;
        if ((now - e->ts) > ARP_TTL_SEC) { e->valid = false; continue; }
        if (memcmp(e->ip, ip, 4) == 0) return e;
    }
    return NULL;
}

static void arp_insert(const u8 ip[4], const u8 mac[6]) {
    /* If we already have an entry for this IP, refresh it. */
    ArpEntry *e = arp_find(ip);
    if (!e) {
        /* Pick the oldest slot (or first invalid). */
        ArpEntry *victim = &s_arp[0];
        for (int i = 1; i < ARP_CACHE_SIZE; i++) {
            if (!s_arp[i].valid) { victim = &s_arp[i]; break; }
            if (s_arp[i].ts < victim->ts) victim = &s_arp[i];
        }
        e = victim;
        memcpy(e->ip, ip, 4);
    }
    memcpy(e->mac, mac, 6);
    e->ts    = time(NULL);
    e->valid = true;
}

/* ---------------------------------------------------------------------------
 * Frame builders
 * ------------------------------------------------------------------------- */

#define ETH_TYPE_IPV4 0x0800
#define ETH_TYPE_ARP  0x0806

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17

/* W5100S Sn_SR codes — mirror net4cpc.c so the stack can drive
 * net4cpc.c's state machine via the on_state callback. */
#define SSTAT_CLOSED      0x00u
#define SSTAT_INIT        0x13u
#define SSTAT_LISTEN      0x14u
#define SSTAT_SYNSENT     0x15u
#define SSTAT_SYNRECV     0x16u
#define SSTAT_ESTABLISHED 0x17u
#define SSTAT_FIN_WAIT    0x18u
#define SSTAT_TIME_WAIT   0x1Bu
#define SSTAT_CLOSE_WAIT  0x1Cu
#define SSTAT_LAST_ACK    0x1Du

#define TCP_FIN  0x01u
#define TCP_SYN  0x02u
#define TCP_RST  0x04u
#define TCP_PSH  0x08u
#define TCP_ACK  0x10u

#define TCP_DEFAULT_MSS 536      /* RFC 1122 minimum that doesn't require PMTUD */
#define TCP_RETX_FRAMES 50       /* ~1s @ 50 fps before re-sending */
#define TCP_RETX_MAX    4        /* fire TIMEOUT in Sn_IR after this many */
#define TCP_RETX_BUF    1460     /* one MSS-sized unacked window per socket  */

static const u8 BCAST_MAC[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

/* ---------------------------------------------------------------------------
 * TCP per-socket control block
 * ------------------------------------------------------------------------- */

typedef struct {
    bool active;
    u8   state;             /* one of SSTAT_*                              */
    u16  local_port;
    u8   peer_ip[4];
    u16  peer_port;
    u8   peer_mac[6];
    bool peer_mac_known;
    u32  snd_una;           /* oldest unacked SEQ                          */
    u32  snd_nxt;           /* next SEQ to send                            */
    u32  rcv_nxt;           /* next expected SEQ from peer                 */
    u16  mss;
    /* Retransmit buffer: bytes we sent but haven't seen ACK for.
     * Held verbatim so retransmission is a memcpy + new IP/TCP headers. */
    u8   retx_buf[TCP_RETX_BUF];
    u16  retx_len;
    u32  retx_seq;          /* SEQ of retx_buf[0]                          */
    int  retx_timer;        /* frames until next retransmit attempt        */
    int  retx_count;        /* retries so far for the current segment      */
    bool fin_sent;          /* we've sent our FIN — remember the SEQ slot  */
} TcpCb;

static TcpCb s_tcp[4];

static const char *tcp_state_name(u8 sr) {
    switch (sr) {
    case 0x00: return "CLOSED";
    case 0x13: return "INIT";
    case 0x14: return "LISTEN";
    case 0x15: return "SYN_SENT";
    case 0x16: return "SYN_RECV";
    case 0x17: return "ESTABLISHED";
    case 0x18: return "FIN_WAIT";
    case 0x1B: return "TIME_WAIT";
    case 0x1C: return "CLOSE_WAIT";
    case 0x1D: return "LAST_ACK";
    default:   return "?";
    }
}

static void tcp_set_state(int s, u8 new_sr) {
    u8 old = s_tcp[s].state;
    s_tcp[s].state = new_sr;
    if (old != new_sr)
        TLOG("TCP[%d] state %s -> %s\n", s,
             tcp_state_name(old), tcp_state_name(new_sr));
    if (s_tcp_state) s_tcp_state(s, new_sr);
}

static void put_u16_be(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static u16  get_u16_be(const u8 *p)   { return (u16)((p[0] << 8) | p[1]); }

/* Build the 14-byte Ethernet header at p. */
static void build_eth(u8 *p, const u8 dst[6], const u8 src[6], u16 etype) {
    memcpy(p,     dst, 6);
    memcpy(p + 6, src, 6);
    put_u16_be(p + 12, etype);
}

/* Send a raw frame to the TAP. Returns bytes written or -1. */
static int tap_tx(const u8 *frame, int len) {
    if (s_tap_fd < 0) return -1;
    leds_ping(LED_NET);
    return tap_write(s_tap_fd, frame, (size_t)len);
}

/* ---------------------------------------------------------------------------
 * ARP
 * ------------------------------------------------------------------------- */

static void send_arp_request(const u8 target_ip[4]) {
    TLOG("ARP -> who-has %u.%u.%u.%u tell %u.%u.%u.%u\n",
         target_ip[0], target_ip[1], target_ip[2], target_ip[3],
         s_sipr[0],    s_sipr[1],    s_sipr[2],    s_sipr[3]);
    /* 14 Eth + 28 ARP = 42 bytes; bring up to minimum 60 for safety. */
    u8 f[60];
    memset(f, 0, sizeof(f));
    build_eth(f, BCAST_MAC, s_mac, ETH_TYPE_ARP);
    u8 *a = f + 14;
    put_u16_be(a + 0, 0x0001);            /* HTYPE Ethernet */
    put_u16_be(a + 2, ETH_TYPE_IPV4);     /* PTYPE IPv4     */
    a[4] = 6;                             /* HLEN */
    a[5] = 4;                             /* PLEN */
    put_u16_be(a + 6, ARP_OP_REQUEST);
    memcpy(a +  8, s_mac,  6);            /* sender hw  */
    memcpy(a + 14, s_sipr, 4);            /* sender pr  */
    /* target hw is zeroed (we don't know it — that's the point) */
    memcpy(a + 24, target_ip, 4);         /* target pr  */
    tap_tx(f, 60);
}

static void send_arp_reply(const u8 target_mac[6], const u8 target_ip[4]) {
    TLOG("ARP -> reply  %u.%u.%u.%u is-at %02X:%02X:%02X:%02X:%02X:%02X\n",
         s_sipr[0], s_sipr[1], s_sipr[2], s_sipr[3],
         s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    (void)target_ip; (void)target_mac;
    u8 f[60];
    memset(f, 0, sizeof(f));
    build_eth(f, target_mac, s_mac, ETH_TYPE_ARP);
    u8 *a = f + 14;
    put_u16_be(a + 0, 0x0001);
    put_u16_be(a + 2, ETH_TYPE_IPV4);
    a[4] = 6; a[5] = 4;
    put_u16_be(a + 6, ARP_OP_REPLY);
    memcpy(a +  8, s_mac,      6);
    memcpy(a + 14, s_sipr,     4);
    memcpy(a + 18, target_mac, 6);
    memcpy(a + 24, target_ip,  4);
    tap_tx(f, 60);
}

/* Sub-millisecond ARP wait: when arp_resolve has just fired a request,
 * give the host kernel a few polls to push the reply back through the
 * TAP before declaring failure. The next-hop IP for off-subnet traffic
 * is our own TAP's host endpoint, so the round trip is local-kernel
 * fast — without this retry the caller (KCNet PING, NCFG -a) sees a
 * spurious "Send ARP Error" on the very first packet. (#130) */
static bool arp_resolve_with_retry(const u8 dst_ip[4], u8 out_mac[6]);

bool n4c_stack_arp_resolve(const u8 dst_ip[4], u8 out_mac[6]) {
    if (!n4c_stack_active()) return false;

    /* Choose the next-hop IP: dst itself if on our subnet, else the
     * gateway. (s_subr & dst) == (s_subr & sipr) means same subnet. */
    u8 hop_ip[4];
    bool on_subnet = true;
    for (int i = 0; i < 4; i++)
        if ((s_subr[i] & dst_ip[i]) != (s_subr[i] & s_sipr[i])) {
            on_subnet = false; break;
        }
    if (on_subnet) memcpy(hop_ip, dst_ip, 4);
    else           memcpy(hop_ip, s_gar,  4);

    /* Special case: broadcast destination. We could send straight to
     * BCAST_MAC, but DHCP-like flows actually want the real broadcast
     * MAC — which is fine here. */
    if (dst_ip[0] == 0xFF && dst_ip[1] == 0xFF &&
        dst_ip[2] == 0xFF && dst_ip[3] == 0xFF) {
        memcpy(out_mac, BCAST_MAC, 6);
        return true;
    }

    ArpEntry *e = arp_find(hop_ip);
    if (e) { memcpy(out_mac, e->mac, 6); return true; }
    send_arp_request(hop_ip);
    return false;
}

static bool arp_resolve_with_retry(const u8 dst_ip[4], u8 out_mac[6]) {
    if (n4c_stack_arp_resolve(dst_ip, out_mac))
        return true;
    /* arp_resolve has fired the request. Drain the TAP a handful of times
     * looking for the reply. Each n4c_stack_poll drains one frame (or
     * returns immediately if none available), so we cap iterations rather
     * than wall-clock. ~32 iterations comfortably covers a local-kernel
     * round trip without blocking the emulator visibly. */
    for (int i = 0; i < 32; i++) {
        n4c_stack_poll();
        if (n4c_stack_arp_resolve(dst_ip, out_mac))
            return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * Inbound dispatch
 * ------------------------------------------------------------------------- */

/* IPv4 header checksum over n bytes (n is the header length, must be even). */
static u16 ip_checksum(const u8 *hdr, int n) {
    u32 sum = 0;
    for (int i = 0; i + 1 < n; i += 2)
        sum += (u32)((hdr[i] << 8) | hdr[i+1]);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

/* UDP checksum: pseudo-header + UDP header + payload. */
static u16 udp_checksum(const u8 src_ip[4], const u8 dst_ip[4],
                       const u8 *udp, int udp_len) {
    u32 sum = 0;
    /* pseudo-header */
    sum += (src_ip[0] << 8) | src_ip[1];
    sum += (src_ip[2] << 8) | src_ip[3];
    sum += (dst_ip[0] << 8) | dst_ip[1];
    sum += (dst_ip[2] << 8) | dst_ip[3];
    sum += IP_PROTO_UDP;
    sum += udp_len;
    /* udp header + payload (pad odd byte with zero) */
    for (int i = 0; i + 1 < udp_len; i += 2)
        sum += (udp[i] << 8) | udp[i+1];
    if (udp_len & 1) sum += (udp[udp_len - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    u16 cs = (u16)~sum;
    return cs ? cs : 0xFFFF;   /* 0 means "no checksum" in UDP; avoid that */
}

static void handle_arp(const u8 *eth, int len) {
    if (len < 14 + 28) return;
    const u8 *a = eth + 14;
    u16 op = get_u16_be(a + 6);
    const u8 *sender_mac = a + 8;
    const u8 *sender_ip  = a + 14;
    const u8 *target_ip  = a + 24;

    /* Always cache the sender — it's a free entry. */
    arp_insert(sender_ip, sender_mac);

    if (op == ARP_OP_REQUEST) {
        bool for_me = (memcmp(target_ip, s_sipr, 4) == 0);
        bool sipr_set = (s_sipr[0]|s_sipr[1]|s_sipr[2]|s_sipr[3]) != 0;
        TLOG("ARP <- who-has %u.%u.%u.%u tell %u.%u.%u.%u  %s\n",
             target_ip[0], target_ip[1], target_ip[2], target_ip[3],
             sender_ip[0], sender_ip[1], sender_ip[2], sender_ip[3],
             for_me  ? "(for me — replying)" :
             !sipr_set ? "(SIPR=0.0.0.0, ignoring)" : "(not for me)");
        if (for_me) send_arp_reply(sender_mac, sender_ip);
    } else if (op == ARP_OP_REPLY) {
        TLOG("ARP <- reply  %u.%u.%u.%u is-at %02X:%02X:%02X:%02X:%02X:%02X\n",
             sender_ip[0], sender_ip[1], sender_ip[2], sender_ip[3],
             sender_mac[0], sender_mac[1], sender_mac[2],
             sender_mac[3], sender_mac[4], sender_mac[5]);
    }
}

static void handle_icmp(const u8 *src_ip,
                        const u8 *icmp, int icmp_len,
                        const u8 *src_mac);
static void handle_udp(const u8 *src_ip, const u8 *dst_ip,
                       const u8 *udp, int udp_len);
static void handle_tcp(const u8 *src_ip, const u8 *dst_ip,
                       const u8 *tcp, int tcp_total_len,
                       const u8 *src_mac);
static void tcp_tick(void);

static void handle_ipv4(const u8 *eth, int len) {
    if (len < 14 + 20) return;
    const u8 *ip = eth + 14;
    u8  ihl   = (ip[0] & 0x0F) * 4;
    u16 total = get_u16_be(ip + 2);
    u8  proto = ip[9];
    const u8 *src_ip = ip + 12;
    const u8 *dst_ip = ip + 16;

    if (ihl < 20) return;
    if (total > len - 14) return;
    /* Accept if dst is broadcast, or matches our SIPR, OR our SIPR is
     * still 0.0.0.0 (DHCP-client pre-configuration case — the host
     * kernel already MAC-filtered the frame to us, so any IP it carries
     * is presumed for us). */
    bool sipr_set = (s_sipr[0] | s_sipr[1] | s_sipr[2] | s_sipr[3]) != 0;
    bool is_bcast = (dst_ip[0] == 0xFF && dst_ip[1] == 0xFF &&
                     dst_ip[2] == 0xFF && dst_ip[3] == 0xFF);
    if (sipr_set && !is_bcast && memcmp(dst_ip, s_sipr, 4) != 0)
        return;   /* not for us */

    /* Cache the source MAC so we can reply without a fresh ARP. */
    arp_insert(src_ip, eth + 6);

    const u8 *payload = ip + ihl;
    int payload_len   = total - ihl;

    /* Raw-IP deliver hook (W5100S IPRAW sockets). Suppresses the
     * default per-protocol handling for this frame when claimed —
     * e.g. PING.COM consumes ICMP echo replies through here. */
    if (s_raw_ip_deliver &&
        s_raw_ip_deliver(proto, src_ip,
                         payload, (u16)payload_len))
        return;

    switch (proto) {
    case IP_PROTO_ICMP: handle_icmp(src_ip, payload, payload_len, eth + 6); break;
    case IP_PROTO_UDP:  handle_udp (src_ip, dst_ip, payload, payload_len); break;
    case IP_PROTO_TCP:  handle_tcp (src_ip, dst_ip, payload, payload_len, eth + 6); break;
    default: break;
    }
}

static void handle_icmp(const u8 *src_ip,
                        const u8 *icmp, int icmp_len,
                        const u8 *src_mac) {
    if (icmp_len < 8) return;
    u8 type = icmp[0];
    if (type != 8 /* echo request */) return;

    /* Build echo reply: same payload, type=0, recompute checksum,
     * wrap in IP header, wrap in Ethernet. */
    u8 frame[TAP_FRAME_MAX];
    int ip_total = 20 + icmp_len;
    if (14 + ip_total > (int)sizeof(frame)) return;

    build_eth(frame, src_mac, s_mac, ETH_TYPE_IPV4);

    u8 *ip = frame + 14;
    ip[0] = 0x45;                    /* IHL=5, ver=4 */
    ip[1] = 0;                       /* DSCP */
    put_u16_be(ip + 2, (u16)ip_total);
    put_u16_be(ip + 4, 0);           /* id */
    put_u16_be(ip + 6, 0x4000);      /* DF, no offset */
    ip[8]  = 64;                     /* TTL */
    ip[9]  = IP_PROTO_ICMP;
    put_u16_be(ip + 10, 0);          /* checksum (will compute) */
    memcpy(ip + 12, s_sipr, 4);
    memcpy(ip + 16, src_ip, 4);
    put_u16_be(ip + 10, ip_checksum(ip, 20));

    u8 *r = ip + 20;
    memcpy(r, icmp, icmp_len);
    r[0] = 0;                        /* echo reply */
    r[1] = 0;
    put_u16_be(r + 2, 0);            /* checksum field cleared */
    /* ICMP checksum: 16-bit ones-complement over header + data. */
    u32 sum = 0;
    for (int i = 0; i + 1 < icmp_len; i += 2)
        sum += (r[i] << 8) | r[i+1];
    if (icmp_len & 1) sum += r[icmp_len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    put_u16_be(r + 2, (u16)~sum);

    TLOG("ICMP -> echo reply to %u.%u.%u.%u (%d bytes)\n",
         src_ip[0], src_ip[1], src_ip[2], src_ip[3], icmp_len);
    tap_tx(frame, 14 + ip_total);
}

/* ---------------------------------------------------------------------------
 * Built-in DHCP server (RFC 2131 minimal).
 *
 * Hardcoded single-client topology so users don't need dnsmasq:
 *   server IP    = 10.0.0.1   (the host side of the tap)
 *   client lease = 10.0.0.100
 *   netmask      = 255.255.255.0
 *   router/dns   = 10.0.0.1
 *   lease time   = 1 day
 *
 * The server runs only when explicitly enabled via
 * n4c_stack_set_dhcp_enabled() — main.c flips it on when
 * cfg->net4cpc_tap is true. When enabled we shortcut DHCP traffic
 * before the W5100S deliver path so the chip never sees its own DHCP
 * client traffic looping back.
 * ------------------------------------------------------------------------- */

static bool s_dhcp_enabled = false;
/* Configurable lease parameters — defaults match the original hardcoded
 * 10.0.0.0/24 topology. Overwritten by n4c_stack_set_dhcp_params().
 * DHCP_CLIENT_IP advances through [lease_start..lease_end] as new MACs
 * appear, but a single-CPC session always sees the first slot. */
static u8 DHCP_SERVER_IP   [4] = { 10, 0, 0, 1   };
static u8 DHCP_NETMASK     [4] = { 255,255,255,0 };
static u8 DHCP_LEASE_START [4] = { 10, 0, 0, 100 };
static u8 DHCP_LEASE_END   [4] = { 10, 0, 0, 150 };
/* Tracks the next IP to hand out. Cleared back to LEASE_START on every
 * set_dhcp_params() so config changes pick up the new range immediately. */
static u8 DHCP_CLIENT_IP   [4] = { 10, 0, 0, 100 };

void n4c_stack_set_dhcp_enabled(bool on) { s_dhcp_enabled = on; }

static bool parse_dotted_quad(const char *s, u8 out[4]) {
    unsigned a, b, c, d;
    if (!s || sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    out[0] = (u8)a; out[1] = (u8)b; out[2] = (u8)c; out[3] = (u8)d;
    return true;
}

void n4c_stack_set_dhcp_params(const char *host_ip,
                               const char *netmask,
                               const char *lease_start,
                               const char *lease_end) {
    parse_dotted_quad(host_ip,     DHCP_SERVER_IP);
    parse_dotted_quad(netmask,     DHCP_NETMASK);
    parse_dotted_quad(lease_start, DHCP_LEASE_START);
    parse_dotted_quad(lease_end,   DHCP_LEASE_END);
    memcpy(DHCP_CLIENT_IP, DHCP_LEASE_START, 4);
}

/* ---------------------------------------------------------------------------
 * Built-in DNS proxy.
 *
 * The DHCP server advertises 10.0.0.1 as the DNS server but no real
 * resolver listens on that address. When the chip sends a DNS query
 * we forward it to the host's upstream (read from /etc/resolv.conf
 * at first use; fall back to 8.8.8.8 if that fails) using a POSIX
 * UDP socket, wait up to 500 ms for the reply, and push it back into
 * the chip's RX buffer via the deliver callback. Synchronous because
 * single-client and reply latency is tolerable; making this async
 * would need a worker thread, which isn't worth it for now.
 * ------------------------------------------------------------------------- */

#if defined(__linux__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>
#endif

static bool s_dns_enabled = false;
static u8   s_dns_upstream[4] = { 8, 8, 8, 8 };
static bool s_dns_upstream_loaded = false;

void n4c_stack_set_dns_enabled(bool on) { s_dns_enabled = on; }

static void dns_load_upstream(void) {
    if (s_dns_upstream_loaded) return;
    s_dns_upstream_loaded = true;
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (strncmp(p, "nameserver", 10) != 0) continue;
        p += 10;
        while (*p == ' ' || *p == '\t') p++;
        unsigned a, b, c, d;
        if (sscanf(p, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
            a < 256 && b < 256 && c < 256 && d < 256) {
            /* Skip self-referential resolvers (the proxied address itself
             * or stub resolvers on loopback) so we don't ping-pong. */
            if (a == 127 || (a == 10 && b == 0 && c == 0 && d == 1)) continue;
            s_dns_upstream[0] = (u8)a; s_dns_upstream[1] = (u8)b;
            s_dns_upstream[2] = (u8)c; s_dns_upstream[3] = (u8)d;
            break;
        }
    }
    fclose(f);
}

/* Forward a query to upstream and deliver the reply back into the
 * chip's RX buffer. Returns true if we handled the request (regardless
 * of whether the upstream replied). */
#if defined(__linux__)
static bool dns_try_handle(u16 src_port, const u8 *query, u16 query_len) {
    if (!s_dns_enabled || query_len < 12) return false;
    dns_load_upstream();

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return true;        /* swallow: nothing else can answer */

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(53);
    memcpy(&sa.sin_addr, s_dns_upstream, 4);
    if (sendto(fd, query, query_len, 0,
               (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return true;
    }

    struct pollfd pfd = { fd, POLLIN, 0 };
    if (poll(&pfd, 1, 500) <= 0) { close(fd); return true; }

    u8 reply[1500];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);
    ssize_t n = recvfrom(fd, reply, sizeof(reply), 0,
                        (struct sockaddr *)&from, &fromlen);
    close(fd);
    if (n <= 0) return true;

    /* Push back into the chip's S? UDP socket bound to src_port. The
     * source IP must be the 10.0.0.1 we advertised so NCFG's resolver
     * accepts it. */
    static const u8 GW[4] = { 10, 0, 0, 1 };
    if (s_udp_deliver)
        s_udp_deliver(GW, 53, src_port, reply, (u16)n);
    TLOG("DNS  -> upstream %u.%u.%u.%u, %u-byte reply delivered\n",
         s_dns_upstream[0], s_dns_upstream[1],
         s_dns_upstream[2], s_dns_upstream[3], (unsigned)n);
    return true;
}
#else  /* non-Linux: TAP backend is unavailable, so DNS proxy is too */
static bool dns_try_handle(u16 src_port, const u8 *query, u16 query_len) {
    (void)src_port; (void)query; (void)query_len;
    return false;
}
#endif

static int dhcp_find_option(const u8 *opts, int opts_len, u8 want, u8 *out, int out_max) {
    int i = 0;
    while (i < opts_len) {
        u8 code = opts[i++];
        if (code == 0) continue;
        if (code == 255) return -1;
        if (i >= opts_len) return -1;
        u8 len = opts[i++];
        if (i + len > opts_len) return -1;
        if (code == want) {
            int n = (len < out_max) ? len : out_max;
            if (out && n > 0) memcpy(out, opts + i, n);
            return len;
        }
        i += len;
    }
    return -1;
}

static void dhcp_send_reply(const u8 *req_bootp, int req_len,
                            u8 msg_type, const u8 client_mac[6]) {
    /* Build reply payload: BOOTP header (236) + magic (4) + options. */
    u8 reply[576];
    memset(reply, 0, sizeof(reply));
    reply[0]  = 2;                                 /* op = BOOTREPLY */
    reply[1]  = 1;                                 /* htype = ethernet */
    reply[2]  = 6;                                 /* hlen = 6 */
    reply[3]  = 0;                                 /* hops */
    memcpy(reply + 4, req_bootp + 4, 4);           /* xid */
    memcpy(reply + 8, req_bootp + 8, 4);           /* secs + flags */
    /* ciaddr stays 0; yiaddr = the offer */
    memcpy(reply + 16, DHCP_CLIENT_IP, 4);
    memcpy(reply + 20, DHCP_SERVER_IP, 4);         /* siaddr */
    memcpy(reply + 28, client_mac, 6);             /* chaddr */
    /* magic cookie */
    reply[236] = 0x63; reply[237] = 0x82;
    reply[238] = 0x53; reply[239] = 0x63;
    int o = 240;
    reply[o++] = 53; reply[o++] = 1; reply[o++] = msg_type;          /* msg type */
    reply[o++] = 54; reply[o++] = 4; memcpy(reply+o, DHCP_SERVER_IP, 4); o += 4;
    reply[o++] = 51; reply[o++] = 4;                                 /* lease 1 day */
    reply[o++] = 0; reply[o++] = 1; reply[o++] = 0x51; reply[o++] = 0x80;
    reply[o++] = 1;  reply[o++] = 4; memcpy(reply+o, DHCP_NETMASK, 4); o += 4;
    reply[o++] = 3;  reply[o++] = 4; memcpy(reply+o, DHCP_SERVER_IP, 4); o += 4;
    reply[o++] = 6;  reply[o++] = 4; memcpy(reply+o, DHCP_SERVER_IP, 4); o += 4;
    reply[o++] = 255;                                                /* end */
    int payload_len = o;
    (void)req_len;
    (void)client_mac;

    /* Deliver straight into the W5100S socket bound to port 68 — the
     * chip-side DHCP client. We don't wrap in eth+ip+udp and write to
     * tap because the chip's RX path reads from poll() which sees host
     * → chip frames; chip-emitted broadcasts don't loop back through
     * tap. Calling deliver directly mirrors what handle_udp would do
     * for an inbound frame. */
    if (s_udp_deliver)
        s_udp_deliver(DHCP_SERVER_IP, 67, 68, reply, (u16)payload_len);
    TLOG("DHCP -> %s (%u.%u.%u.%u, lease 10.0.0.100)\n",
         msg_type == 2 ? "OFFER" : msg_type == 5 ? "ACK" : "NAK",
         DHCP_SERVER_IP[0], DHCP_SERVER_IP[1],
         DHCP_SERVER_IP[2], DHCP_SERVER_IP[3]);
}

/* Returns true if the packet was a DHCP request and we replied. */
static bool dhcp_try_handle(const u8 *udp_payload, int payload_len) {
    if (!s_dhcp_enabled) return false;
    if (payload_len < 240) return false;
    if (udp_payload[0] != 1) return false;                /* op = BOOTREQUEST */
    /* Magic cookie check */
    if (udp_payload[236] != 0x63 || udp_payload[237] != 0x82 ||
        udp_payload[238] != 0x53 || udp_payload[239] != 0x63) return false;
    u8 client_mac[6];
    memcpy(client_mac, udp_payload + 28, 6);
    u8 type = 0;
    if (dhcp_find_option(udp_payload + 240, payload_len - 240, 53, &type, 1) != 1)
        return false;
    if (type == 1)        /* DISCOVER */
        dhcp_send_reply(udp_payload, payload_len, 2 /* OFFER */, client_mac);
    else if (type == 3)   /* REQUEST */
        dhcp_send_reply(udp_payload, payload_len, 5 /* ACK */, client_mac);
    else
        return false;     /* DECLINE / RELEASE / INFORM: ignore, no reply */
    return true;
}

static void handle_udp(const u8 *src_ip, const u8 *dst_ip,
                       const u8 *udp, int udp_len) {
    (void)dst_ip;
    if (udp_len < 8) return;
    u16 src_port = get_u16_be(udp + 0);
    u16 dst_port = get_u16_be(udp + 2);
    u16 length   = get_u16_be(udp + 4);
    if (length < 8 || length > udp_len) return;
    const u8 *payload = udp + 8;
    u16 payload_len   = (u16)(length - 8);
    /* Built-in DHCP server: swallow client → server traffic before it
     * reaches the W5100S deliver path. */
    if (dst_port == 67 && dhcp_try_handle(payload, payload_len))
        return;
    int accepted = 0;
    if (s_udp_deliver)
        accepted = s_udp_deliver(src_ip, src_port, dst_port, payload, payload_len);
    TLOG("UDP <- %u.%u.%u.%u:%u -> :%u  %u bytes  %s\n",
         src_ip[0], src_ip[1], src_ip[2], src_ip[3], src_port,
         dst_port, payload_len,
         accepted ? "delivered" : "no-matching-socket");
}

void n4c_stack_poll(void) {
    if (s_tap_fd < 0) return;
    static int first = 1;
    if (first && n4c_stack_trace) {
        first = 0;
        fprintf(stderr, "[n4c] poll: first call, s_tap_fd=%d\n", s_tap_fd);
    }
    u8 frame[TAP_FRAME_MAX];
    for (int drained = 0; drained < 64; drained++) {
        int n = tap_read(s_tap_fd, frame, sizeof(frame));
        if (n <= 0) break;
        leds_ping(LED_NET);
        if (n < 14) continue;
        u16 etype = get_u16_be(frame + 12);
        if (n4c_stack_trace) {
            fprintf(stderr, "[n4c] poll: frame %d bytes  dst=%02X:%02X:%02X:%02X:%02X:%02X "
                "src=%02X:%02X:%02X:%02X:%02X:%02X etype=%04X\n",
                n,
                frame[0], frame[1], frame[2], frame[3], frame[4], frame[5],
                frame[6], frame[7], frame[8], frame[9], frame[10], frame[11],
                etype);
        }
        switch (etype) {
        case ETH_TYPE_ARP:  handle_arp (frame, n); break;
        case ETH_TYPE_IPV4: handle_ipv4(frame, n); break;
        default: break;
        }
    }
    tcp_tick();
}

/* ---------------------------------------------------------------------------
 * Outbound UDP
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * TCP helpers
 * ------------------------------------------------------------------------- */

static void put_u32_be(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >>  8); p[3] = (u8) v;
}
static u32 get_u32_be(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] <<  8) |  (u32)p[3];
}

static u16 tcp_checksum(const u8 src_ip[4], const u8 dst_ip[4],
                        const u8 *tcp, int tcp_len) {
    u32 sum = 0;
    sum += (src_ip[0] << 8) | src_ip[1];
    sum += (src_ip[2] << 8) | src_ip[3];
    sum += (dst_ip[0] << 8) | dst_ip[1];
    sum += (dst_ip[2] << 8) | dst_ip[3];
    sum += IP_PROTO_TCP;
    sum += tcp_len;
    for (int i = 0; i + 1 < tcp_len; i += 2)
        sum += (tcp[i] << 8) | tcp[i+1];
    if (tcp_len & 1) sum += tcp[tcp_len - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

/* Build + send a TCP segment from socket s with the given flags and
 * payload. Uses the current snd_nxt and rcv_nxt. Updates snd_nxt by
 * payload_len + (SYN ? 1 : 0) + (FIN ? 1 : 0). Returns 0 on success,
 * -1 on transport failure (typically: ARP not resolved yet). */
static int tcp_tx(int s, u8 flags, const u8 *payload, int payload_len) {
    TcpCb *cb = &s_tcp[s];
    if (!cb->active) return -1;

    if (!cb->peer_mac_known) {
        if (!n4c_stack_arp_resolve(cb->peer_ip, cb->peer_mac))
            return -1;
        cb->peer_mac_known = true;
    }

    u8 frame[TAP_FRAME_MAX];
    int tcp_len  = 20 + payload_len;
    int ip_total = 20 + tcp_len;
    int eth_len  = 14 + ip_total;
    if (eth_len > TAP_FRAME_MAX) return -1;

    build_eth(frame, cb->peer_mac, s_mac, ETH_TYPE_IPV4);

    u8 *ip = frame + 14;
    ip[0] = 0x45;
    ip[1] = 0;
    put_u16_be(ip + 2, (u16)ip_total);
    put_u16_be(ip + 4, 0);            /* id (we don't fragment) */
    put_u16_be(ip + 6, 0x4000);       /* DF set, fragment offset 0 */
    ip[8]  = 64;
    ip[9]  = IP_PROTO_TCP;
    put_u16_be(ip + 10, 0);
    memcpy(ip + 12, s_sipr,    4);
    memcpy(ip + 16, cb->peer_ip, 4);
    put_u16_be(ip + 10, ip_checksum(ip, 20));

    u8 *tcp = ip + 20;
    put_u16_be(tcp + 0,  cb->local_port);
    put_u16_be(tcp + 2,  cb->peer_port);
    put_u32_be(tcp + 4,  cb->snd_nxt);
    put_u32_be(tcp + 8,  cb->rcv_nxt);
    tcp[12] = 0x50;                   /* data offset = 5 (20 bytes), reserved=0 */
    tcp[13] = flags;
    put_u16_be(tcp + 14, 2048);       /* window: we have 2 KB RX rings */
    put_u16_be(tcp + 16, 0);          /* checksum (filled below) */
    put_u16_be(tcp + 18, 0);          /* urgent */
    if (payload_len > 0)
        memcpy(tcp + 20, payload, payload_len);
    put_u16_be(tcp + 16, tcp_checksum(s_sipr, cb->peer_ip, tcp, tcp_len));

    if (tap_tx(frame, eth_len) < 0) return -1;

    if (n4c_stack_trace) {
        char fl[16] = "";
        if (flags & TCP_SYN) strcat(fl, "S");
        if (flags & TCP_ACK) strcat(fl, "A");
        if (flags & TCP_PSH) strcat(fl, "P");
        if (flags & TCP_FIN) strcat(fl, "F");
        if (flags & TCP_RST) strcat(fl, "R");
        TLOG("TCP[%d] -> %u.%u.%u.%u:%u [%s] seq=%u ack=%u %d bytes\n",
             s, cb->peer_ip[0], cb->peer_ip[1], cb->peer_ip[2], cb->peer_ip[3],
             cb->peer_port, fl, cb->snd_nxt, cb->rcv_nxt, payload_len);
    }

    cb->snd_nxt += payload_len;
    if (flags & TCP_SYN) cb->snd_nxt += 1;
    if (flags & TCP_FIN) cb->snd_nxt += 1;
    return 0;
}

/* Send a bare RST in response to a stray segment. Doesn't touch any CB. */
static void tcp_send_rst(const u8 dst_ip[4], u16 dst_port,
                        u16 src_port, u32 seq, u32 ack) {
    u8 dst_mac[6];
    if (!n4c_stack_arp_resolve(dst_ip, dst_mac)) return;
    u8 frame[60];
    memset(frame, 0, sizeof(frame));
    build_eth(frame, dst_mac, s_mac, ETH_TYPE_IPV4);
    u8 *ip = frame + 14;
    ip[0] = 0x45; ip[1] = 0;
    put_u16_be(ip + 2, 40);
    put_u16_be(ip + 4, 0);
    put_u16_be(ip + 6, 0x4000);
    ip[8] = 64; ip[9] = IP_PROTO_TCP;
    put_u16_be(ip + 10, 0);
    memcpy(ip + 12, s_sipr, 4);
    memcpy(ip + 16, dst_ip, 4);
    put_u16_be(ip + 10, ip_checksum(ip, 20));
    u8 *tcp = ip + 20;
    put_u16_be(tcp + 0, src_port);
    put_u16_be(tcp + 2, dst_port);
    put_u32_be(tcp + 4, seq);
    put_u32_be(tcp + 8, ack);
    tcp[12] = 0x50;
    tcp[13] = TCP_RST | TCP_ACK;
    put_u16_be(tcp + 14, 0);
    put_u16_be(tcp + 16, 0);
    put_u16_be(tcp + 18, 0);
    put_u16_be(tcp + 16, tcp_checksum(s_sipr, dst_ip, tcp, 20));
    tap_tx(frame, 60);
}

/* ---------------------------------------------------------------------------
 * TCP public API (called from net4cpc.c)
 * ------------------------------------------------------------------------- */

void n4c_stack_tcp_open(int s, u16 local_port) {
    if (s < 0 || s > 3) return;
    TcpCb *cb = &s_tcp[s];
    memset(cb, 0, sizeof(*cb));
    cb->active     = true;
    cb->state      = SSTAT_INIT;
    cb->local_port = local_port;
    cb->mss        = TCP_DEFAULT_MSS;
}

void n4c_stack_tcp_listen(int s) {
    if (s < 0 || s > 3) return;
    if (!s_tcp[s].active) return;
    s_tcp[s].state = SSTAT_LISTEN;
}

bool n4c_stack_tcp_connect(int s, const u8 dst_ip[4], u16 dst_port) {
    if (s < 0 || s > 3) return false;
    TcpCb *cb = &s_tcp[s];
    if (!cb->active) return false;
    memcpy(cb->peer_ip, dst_ip, 4);
    cb->peer_port      = dst_port;
    cb->peer_mac_known = false;
    /* Initial sequence number: anything works for correctness; use a
     * trivial hash of (time, ports) to avoid collisions across runs. */
    u32 t = (u32)time(NULL);
    cb->snd_una = t * 7919u + cb->local_port * 31u + dst_port;
    cb->snd_nxt = cb->snd_una;
    cb->rcv_nxt = 0;
    cb->retx_len   = 0;
    cb->retx_timer = TCP_RETX_FRAMES;
    cb->retx_count = 0;
    if (tcp_tx(s, TCP_SYN, NULL, 0) < 0) {
        /* ARP pending — retry in tcp_tick when we poll. */
        cb->state = SSTAT_SYNSENT;  /* still semantically SYN-sending */
        if (s_tcp_state) s_tcp_state(s, SSTAT_SYNSENT);
        return false;
    }
    cb->state = SSTAT_SYNSENT;
    if (s_tcp_state) s_tcp_state(s, SSTAT_SYNSENT);
    return true;
}

int n4c_stack_tcp_send(int s, const u8 *data, int len) {
    if (s < 0 || s > 3) return 0;
    TcpCb *cb = &s_tcp[s];
    if (!cb->active || cb->state != SSTAT_ESTABLISHED) return 0;
    /* For simplicity hold ONE outstanding segment; sender (kernel TX
     * FSR) won't refill until SEND_OK fires. */
    int seg = len;
    if (seg > cb->mss)              seg = cb->mss;
    if (seg > (int)sizeof(cb->retx_buf)) seg = (int)sizeof(cb->retx_buf);
    memcpy(cb->retx_buf, data, seg);
    cb->retx_len   = (u16)seg;
    cb->retx_seq   = cb->snd_nxt;
    cb->retx_timer = TCP_RETX_FRAMES;
    cb->retx_count = 0;
    tcp_tx(s, TCP_ACK | TCP_PSH, cb->retx_buf, seg);
    return seg;
}

void n4c_stack_tcp_disconnect(int s) {
    if (s < 0 || s > 3) return;
    TcpCb *cb = &s_tcp[s];
    if (!cb->active) return;
    if (cb->state == SSTAT_ESTABLISHED) {
        tcp_tx(s, TCP_FIN | TCP_ACK, NULL, 0);
        cb->fin_sent = true;
        cb->state    = SSTAT_FIN_WAIT;
        if (s_tcp_state) s_tcp_state(s, SSTAT_FIN_WAIT);
    } else if (cb->state == SSTAT_CLOSE_WAIT) {
        tcp_tx(s, TCP_FIN | TCP_ACK, NULL, 0);
        cb->fin_sent = true;
        cb->state    = SSTAT_LAST_ACK;
        if (s_tcp_state) s_tcp_state(s, SSTAT_LAST_ACK);
    }
}

void n4c_stack_tcp_close(int s) {
    if (s < 0 || s > 3) return;
    TcpCb *cb = &s_tcp[s];
    if (!cb->active) return;
    if (cb->peer_mac_known && cb->state != SSTAT_CLOSED) {
        tcp_send_rst(cb->peer_ip, cb->peer_port,
                     cb->local_port, cb->snd_nxt, 0);
    }
    cb->active = false;
    cb->state  = SSTAT_CLOSED;
    if (s_tcp_state) s_tcp_state(s, SSTAT_CLOSED);
}

/* ---------------------------------------------------------------------------
 * TCP inbound dispatcher + retransmit tick
 * ------------------------------------------------------------------------- */

static TcpCb *tcp_find(u16 local_port,
                      const u8 peer_ip[4], u16 peer_port) {
    TcpCb *listener = NULL;
    for (int i = 0; i < 4; i++) {
        TcpCb *c = &s_tcp[i];
        if (!c->active) continue;
        if (c->local_port != local_port) continue;
        if (c->state == SSTAT_LISTEN) { listener = c; continue; }
        if (memcmp(c->peer_ip, peer_ip, 4) == 0 &&
            c->peer_port == peer_port)
            return c;
    }
    return listener;
}

static int cb_index(TcpCb *cb) {
    return (int)(cb - s_tcp);
}

static void handle_tcp(const u8 *src_ip, const u8 *dst_ip,
                       const u8 *tcp, int tcp_total_len,
                       const u8 *src_mac) {
    (void)dst_ip;
    if (tcp_total_len < 20) return;
    u16 src_port = get_u16_be(tcp + 0);
    u16 dst_port = get_u16_be(tcp + 2);
    u32 seq      = get_u32_be(tcp + 4);
    u32 ack      = get_u32_be(tcp + 8);
    int hdr_len  = (tcp[12] >> 4) * 4;
    u8  flags    = tcp[13];
    if (hdr_len < 20 || hdr_len > tcp_total_len) return;

    const u8 *payload = tcp + hdr_len;
    int payload_len   = tcp_total_len - hdr_len;

    TcpCb *cb = tcp_find(dst_port, src_ip, src_port);
    if (!cb) {
        if (!(flags & TCP_RST))
            tcp_send_rst(src_ip, src_port, dst_port, ack, seq + 1);
        return;
    }
    int s = cb_index(cb);

    if (flags & TCP_RST) {
        cb->active = false;
        tcp_set_state(s, SSTAT_CLOSED);
        return;
    }

    switch (cb->state) {
    case SSTAT_LISTEN:
        if (flags & TCP_SYN) {
            memcpy(cb->peer_ip, src_ip, 4);
            cb->peer_port      = src_port;
            memcpy(cb->peer_mac, src_mac, 6);
            cb->peer_mac_known = true;
            cb->rcv_nxt = seq + 1;
            u32 t = (u32)time(NULL);
            cb->snd_una = t * 7919u + cb->local_port * 31u + src_port;
            cb->snd_nxt = cb->snd_una;
            tcp_tx(s, TCP_SYN | TCP_ACK, NULL, 0);
            tcp_set_state(s, SSTAT_SYNRECV);
        }
        break;

    case SSTAT_SYNSENT:
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
            ack == cb->snd_nxt) {
            cb->rcv_nxt = seq + 1;
            cb->snd_una = ack;
            memcpy(cb->peer_mac, src_mac, 6);
            cb->peer_mac_known = true;
            tcp_tx(s, TCP_ACK, NULL, 0);
            tcp_set_state(s, SSTAT_ESTABLISHED);
        } else if (flags & TCP_SYN) {
            /* simultaneous open — go SYN_RECV-ish */
            cb->rcv_nxt = seq + 1;
            tcp_tx(s, TCP_SYN | TCP_ACK, NULL, 0);
            tcp_set_state(s, SSTAT_SYNRECV);
        }
        break;

    case SSTAT_SYNRECV:
        if ((flags & TCP_ACK) && ack == cb->snd_nxt) {
            cb->snd_una = ack;
            tcp_set_state(s, SSTAT_ESTABLISHED);
        }
        break;

    case SSTAT_ESTABLISHED:
    case SSTAT_FIN_WAIT:
    case SSTAT_CLOSE_WAIT:
    case SSTAT_LAST_ACK:
        /* Consume any in-order data. We don't reorder out-of-window
         * segments; the peer's retransmit will sort it out. */
        if (payload_len > 0 && seq == cb->rcv_nxt) {
            int taken = payload_len;
            if (s_tcp_data) taken = s_tcp_data(s, payload, payload_len);
            if (taken > 0) {
                cb->rcv_nxt += (u32)taken;
                tcp_tx(s, TCP_ACK, NULL, 0);
            }
        } else if (payload_len > 0) {
            /* duplicate/out-of-window — just re-ACK so peer notices */
            tcp_tx(s, TCP_ACK, NULL, 0);
        }
        /* ACK accounting */
        if ((flags & TCP_ACK) && ack > cb->snd_una) {
            u32 newly = ack - cb->snd_una;
            cb->snd_una = ack;
            /* If the new ack covers our retx segment, clear it. */
            if (cb->retx_len &&
                ack >= cb->retx_seq + cb->retx_len) {
                u16 done = cb->retx_len;
                cb->retx_len = 0;
                cb->retx_count = 0;
                if (s_tcp_ack) s_tcp_ack(s, done);
            }
            (void)newly;
        }
        /* FIN handling */
        if (flags & TCP_FIN) {
            cb->rcv_nxt += 1;
            tcp_tx(s, TCP_ACK, NULL, 0);
            if (cb->state == SSTAT_ESTABLISHED) {
                tcp_set_state(s, SSTAT_CLOSE_WAIT);
            } else if (cb->state == SSTAT_FIN_WAIT) {
                tcp_set_state(s, SSTAT_TIME_WAIT);
                /* Real TIME_WAIT is 2*MSL; just go to CLOSED for sim. */
                cb->active = false;
                tcp_set_state(s, SSTAT_CLOSED);
            } else if (cb->state == SSTAT_LAST_ACK) {
                cb->active = false;
                tcp_set_state(s, SSTAT_CLOSED);
            }
        }
        /* FIN_WAIT: peer ACKs our FIN -> CLOSED via TIME_WAIT */
        if (cb->state == SSTAT_FIN_WAIT && cb->fin_sent &&
            (flags & TCP_ACK) && ack == cb->snd_nxt) {
            /* still wait for peer's FIN — leave state alone */
        }
        if (cb->state == SSTAT_LAST_ACK && cb->fin_sent &&
            (flags & TCP_ACK) && ack == cb->snd_nxt) {
            cb->active = false;
            tcp_set_state(s, SSTAT_CLOSED);
        }
        break;

    default:
        break;
    }
}

static void tcp_tick(void) {
    for (int s = 0; s < 4; s++) {
        TcpCb *cb = &s_tcp[s];
        if (!cb->active) continue;
        /* Retry the SYN if ARP only just resolved. */
        if (cb->state == SSTAT_SYNSENT && cb->snd_nxt == cb->snd_una) {
            if (cb->retx_timer > 0) cb->retx_timer--;
            if (cb->retx_timer == 0) {
                cb->retx_timer = TCP_RETX_FRAMES;
                if (++cb->retx_count > TCP_RETX_MAX) {
                    if (s_tcp_state) s_tcp_state(s, SSTAT_CLOSED);
                    cb->active = false;
                    continue;
                }
                tcp_tx(s, TCP_SYN, NULL, 0);
                /* SYN logically occupies 1 sequence number. tcp_tx bumped
                 * snd_nxt; for a SYN retransmit we want snd_nxt to stay at
                 * ISN+1 (not ISN+N after N retries) so the inbound SYN-ACK
                 * handler's `ack == snd_nxt` check matches and we send a
                 * proper ACK rather than mis-detecting simultaneous-open
                 * and emitting another SYN-ACK. Pin to snd_una+1, which is
                 * the invariant after the first successful SYN transmit. */
                cb->snd_nxt = cb->snd_una + 1;
            }
        }
        /* Retransmit pending data */
        if (cb->retx_len > 0) {
            if (cb->retx_timer > 0) cb->retx_timer--;
            if (cb->retx_timer == 0) {
                cb->retx_timer = TCP_RETX_FRAMES;
                if (++cb->retx_count > TCP_RETX_MAX) {
                    cb->retx_len = 0;
                    if (s_tcp_state) s_tcp_state(s, SSTAT_CLOSED);
                    cb->active = false;
                    continue;
                }
                /* Re-send from retx_seq */
                u32 saved_nxt = cb->snd_nxt;
                cb->snd_nxt = cb->retx_seq;
                tcp_tx(s, TCP_ACK | TCP_PSH, cb->retx_buf, cb->retx_len);
                cb->snd_nxt = saved_nxt;
            }
        }
    }
}

int n4c_stack_send_udp(u16 src_port,
                       const u8 dst_ip[4], u16 dst_port,
                       const u8 *payload, u16 payload_len) {
    if (!n4c_stack_active()) return -1;

    /* DHCP client → server. The built-in server lives in this process,
     * not behind tap0, so traffic emitted by the chip never loops back
     * via the host. Intercept here and reply synchronously. */
    if (s_dhcp_enabled && dst_port == 67) {
        if (dhcp_try_handle(payload, payload_len))
            return (int)payload_len;
    }
    /* DNS proxy: queries to 10.0.0.1:53 (the DHCP-advertised resolver)
     * are forwarded to the host's upstream. */
    if (s_dns_enabled && dst_port == 53) {
        if (dns_try_handle(src_port, payload, payload_len))
            return (int)payload_len;
    }

    u8 dst_mac[6];
    if (!arp_resolve_with_retry(dst_ip, dst_mac))
        return -1;        /* ARP timed out for real */

    if (payload_len > TAP_FRAME_MAX - 14 - 20 - 8) return -1;

    u8 frame[TAP_FRAME_MAX];
    int udp_len  = 8 + payload_len;
    int ip_total = 20 + udp_len;
    int eth_len  = 14 + ip_total;

    build_eth(frame, dst_mac, s_mac, ETH_TYPE_IPV4);

    u8 *ip = frame + 14;
    ip[0] = 0x45;
    ip[1] = 0;
    put_u16_be(ip + 2, (u16)ip_total);
    put_u16_be(ip + 4, 0);
    put_u16_be(ip + 6, 0x4000);
    ip[8]  = 64;
    ip[9]  = IP_PROTO_UDP;
    put_u16_be(ip + 10, 0);
    memcpy(ip + 12, s_sipr, 4);
    memcpy(ip + 16, dst_ip, 4);
    put_u16_be(ip + 10, ip_checksum(ip, 20));

    u8 *udp = ip + 20;
    put_u16_be(udp + 0, src_port);
    put_u16_be(udp + 2, dst_port);
    put_u16_be(udp + 4, (u16)udp_len);
    put_u16_be(udp + 6, 0);
    memcpy(udp + 8, payload, payload_len);
    put_u16_be(udp + 6, udp_checksum(s_sipr, dst_ip, udp, udp_len));

    if (tap_tx(frame, eth_len) < 0) return -1;
    TLOG("UDP -> %u.%u.%u.%u:%u from :%u  %u bytes\n",
         dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3], dst_port,
         src_port, payload_len);
    return payload_len;
}

int n4c_stack_send_ip(u8 proto, const u8 dst_ip[4],
                      const u8 *payload, u16 payload_len) {
    if (!n4c_stack_active()) return -1;

    u8 dst_mac[6];
    if (!arp_resolve_with_retry(dst_ip, dst_mac))
        return -1;

    if (payload_len > TAP_FRAME_MAX - 14 - 20) return -1;

    u8 frame[TAP_FRAME_MAX];
    int ip_total = 20 + payload_len;
    int eth_len  = 14 + ip_total;

    build_eth(frame, dst_mac, s_mac, ETH_TYPE_IPV4);

    u8 *ip = frame + 14;
    ip[0] = 0x45;
    ip[1] = 0;
    put_u16_be(ip + 2, (u16)ip_total);
    put_u16_be(ip + 4, 0);
    put_u16_be(ip + 6, 0x4000);
    ip[8]  = 64;
    ip[9]  = proto;
    put_u16_be(ip + 10, 0);
    memcpy(ip + 12, s_sipr, 4);
    memcpy(ip + 16, dst_ip, 4);
    put_u16_be(ip + 10, ip_checksum(ip, 20));

    memcpy(ip + 20, payload, payload_len);

    if (tap_tx(frame, eth_len) < 0) return -1;
    TLOG("IP  -> %u.%u.%u.%u proto=%u  %u bytes\n",
         dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3], proto, payload_len);
    return payload_len;
}

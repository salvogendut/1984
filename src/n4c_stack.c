/* n4c_stack.c — see n4c_stack.h
 *
 * This compilation unit owns: the ARP cache, the IPv4 header build/parse,
 * the ICMP echo responder, and the UDP send/route surface. TCP lives in
 * the same file (phase 6 of #104) but is added in a later commit.
 */
#include "n4c_stack.h"
#include "tap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * Live config
 * ------------------------------------------------------------------------- */

static int  s_tap_fd = -1;
static u8   s_mac [6];
static u8   s_sipr[4];
static u8   s_gar [4];
static u8   s_subr[4];

static N4CUdpDeliver s_udp_deliver = NULL;

void n4c_stack_set_udp_deliver(N4CUdpDeliver fn) { s_udp_deliver = fn; }

bool n4c_stack_active(void) {
    if (s_tap_fd < 0) return false;
    /* SIPR all-zero means no IP yet — let the legacy path handle DHCP
     * scenarios until the kernel programs a real address. */
    return (s_sipr[0] | s_sipr[1] | s_sipr[2] | s_sipr[3]) != 0;
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

static const u8 BCAST_MAC[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

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
    return tap_write(s_tap_fd, frame, (size_t)len);
}

/* ---------------------------------------------------------------------------
 * ARP
 * ------------------------------------------------------------------------- */

static void send_arp_request(const u8 target_ip[4]) {
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

    if (op == ARP_OP_REQUEST && memcmp(target_ip, s_sipr, 4) == 0) {
        send_arp_reply(sender_mac, sender_ip);
    }
    /* ARP replies are handled by the cache update above. */
}

static void handle_icmp(const u8 *src_ip,
                        const u8 *icmp, int icmp_len,
                        const u8 *src_mac);
static void handle_udp(const u8 *src_ip, const u8 *dst_ip,
                       const u8 *udp, int udp_len);

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
    if (memcmp(dst_ip, s_sipr, 4) != 0 &&
        !(dst_ip[0] == 0xFF && dst_ip[1] == 0xFF &&
          dst_ip[2] == 0xFF && dst_ip[3] == 0xFF))
        return;   /* not for us */

    /* Cache the source MAC so we can reply without a fresh ARP. */
    arp_insert(src_ip, eth + 6);

    const u8 *payload = ip + ihl;
    int payload_len   = total - ihl;

    switch (proto) {
    case IP_PROTO_ICMP: handle_icmp(src_ip, payload, payload_len, eth + 6); break;
    case IP_PROTO_UDP:  handle_udp (src_ip, dst_ip, payload, payload_len); break;
    /* TCP comes in phase 6 */
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

    tap_tx(frame, 14 + ip_total);
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
    if (s_udp_deliver)
        s_udp_deliver(src_ip, src_port, dst_port, payload, payload_len);
}

void n4c_stack_poll(void) {
    if (s_tap_fd < 0) return;
    u8 frame[TAP_FRAME_MAX];
    for (int drained = 0; drained < 64; drained++) {
        int n = tap_read(s_tap_fd, frame, sizeof(frame));
        if (n <= 0) break;
        if (n < 14) continue;
        u16 etype = get_u16_be(frame + 12);
        switch (etype) {
        case ETH_TYPE_ARP:  handle_arp (frame, n); break;
        case ETH_TYPE_IPV4: handle_ipv4(frame, n); break;
        default: break;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Outbound UDP
 * ------------------------------------------------------------------------- */

int n4c_stack_send_udp(u16 src_port,
                       const u8 dst_ip[4], u16 dst_port,
                       const u8 *payload, u16 payload_len) {
    if (!n4c_stack_active()) return -1;

    u8 dst_mac[6];
    if (!n4c_stack_arp_resolve(dst_ip, dst_mac))
        return -1;        /* ARP query queued; caller retries */

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
    return payload_len;
}

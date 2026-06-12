/* net4cpc.c — W5100S / Net4CPC hardware emulation
 *
 * Emulates the W5100S Ethernet controller as accessed through the Net4CPC
 * expansion board at I/O ports 0xFD20–0xFD23 (indirect parallel bus mode).
 *
 * Register space layout (mirrors real W5100S):
 *   0x0000–0x002F  Common registers (MR, GAR, SUBR, SHAR, SIPR, RTR, …)
 *   0x0400–0x04FF  Socket 0 registers
 *   0x0500–0x05FF  Socket 1 registers
 *   0x0600–0x06FF  Socket 2 registers
 *   0x0700–0x07FF  Socket 3 registers
 *   0x4000–0x5FFF  TX ring buffers (2 KB × 4 sockets)
 *   0x6000–0x7FFF  RX ring buffers (2 KB × 4 sockets)
 *
 * UDP sockets: the W5100S prepends an 8-byte header to each received
 * datagram: 4 bytes source IP, 2 bytes source port, 2 bytes payload length.
 */

#include "net4cpc.h"
#include "tap.h"
#include "n4c_stack.h"
#include "compat_win.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

int net4cpc_trace = 0;

/* TAP backend state. When tap_fd >= 0 and the kernel has programmed a
 * non-zero SIPR, n4c_stack_active() returns true and SCMD_SEND on UDP
 * sockets routes through the L2 stack (n4c_stack.c) instead of the
 * legacy host-POSIX-socket fallback. The fallback is still wired for
 * the no-TAP case and for TCP until phase 6 lands. */
static int  tap_fd        = -1;
static char tap_dev_name[32] = { 0 };

/* Forward decl — defined after the W5100S helpers. */
static int  stack_deliver_udp(const u8 src_ip[4], u16 src_port,
                              u16 dst_port,
                              const u8 *payload, u16 payload_len);
static int  stack_deliver_ip (u8 proto, const u8 src_ip[4],
                              const u8 *payload, u16 payload_len);
static void stack_sync_config(void);
static void stack_tcp_state (int s, u8 new_sr);
static int  stack_tcp_data  (int s, const u8 *data, int len);
static void stack_tcp_ack   (int s, u16 acked);

/* ---------------------------------------------------------------------------
 * W5100S register-space constants
 * ------------------------------------------------------------------------- */

static const u16 SOCK_BASE[4] = { 0x0400, 0x0500, 0x0600, 0x0700 };
static const u16 TX_BASE[4]   = { 0x4000, 0x4800, 0x5000, 0x5800 };
static const u16 RX_BASE[4]   = { 0x6000, 0x6800, 0x7000, 0x7800 };
static const u16 BUF_MASK     = 0x07FFu; /* 2 KB per socket */
static const u16 BUF_SIZE     = 2048u;

/* Socket register offsets from SOCK_BASE[n] */
#define SR_MR     0x00u
#define SR_CR     0x01u
#define SR_IR     0x02u   /* socket interrupt register (RECV=bit2)            */
#define SR_SR     0x03u
#define SR_DIPR   0x0Cu /* 4 bytes, big-endian */
#define SR_DPORT  0x10u /* 2 bytes, big-endian */
#define SR_TX_FSR 0x20u /* 2 bytes */
#define SR_TX_RD  0x22u /* 2 bytes */
#define SR_TX_WR  0x24u /* 2 bytes */
#define SR_RX_RSR 0x26u /* 2 bytes */
#define SR_RX_RD  0x28u /* 2 bytes */

/* Socket modes */
#define SMODE_TCP   0x01u
#define SMODE_UDP   0x02u
#define SMODE_IPRAW 0x03u   /* raw IP — used by KCNet PING.COM for ICMP */

/* IPRAW-specific socket register: Sn_PROTO (the IP protocol number
 * the chip puts in the IP header of outbound packets and matches on
 * inbound). Per W5100S datasheet § 4.2.5. */
#define SR_PROTO 0x14u

/* Socket status */
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
#define SSTAT_UDP         0x22u
#define SSTAT_IPRAW       0x32u

/* Socket commands */
#define SCMD_OPEN    0x01u
#define SCMD_LISTEN  0x02u
#define SCMD_CONNECT 0x04u
#define SCMD_DISCON  0x08u
#define SCMD_CLOSE   0x10u
#define SCMD_SEND    0x20u
#define SCMD_RECV    0x40u

/* ---------------------------------------------------------------------------
 * Emulator state
 * ------------------------------------------------------------------------- */

static u8  regs[65536];   /* W5100S register/buffer space */
static u16 idm_ar;        /* current indirect address register */
static int sock_fd[4];    /* host socket fds (-1 = closed) */
static u16 rx_wr[4];      /* internal RX write pointers (free-running) */

/* TCNTR (0x0082/0x0083): a free-running 100 µs ticker. Reading it latches
 * the current elapsed time; writing 0x0088 (TCNTCLR) restarts it. */
static clock_t tcntr_start;

static inline void tcntr_reset(void) { tcntr_start = clock(); }

static inline u16 tcntr_read(void) {
    double us = (double)(clock() - tcntr_start) * 1e6 / CLOCKS_PER_SEC;
    return (u16)((u32)(us / 10.0) & 0xFFFFu);
}

/* ---------------------------------------------------------------------------
 * Helpers
 * ------------------------------------------------------------------------- */

static void set16(u16 addr, u16 val) {
    regs[addr]   = (u8)(val >> 8);
    regs[addr+1] = (u8)(val & 0xFF);
}

static u16 get16(u16 addr) {
    return (u16)((u16)regs[addr] << 8) | regs[addr+1];
}

static void update_rx_rsr(int s) {
    u16 rsr = (u16)(rx_wr[s] - get16(SOCK_BASE[s] + SR_RX_RD));
    set16(SOCK_BASE[s] + SR_RX_RSR, rsr);
}

static void update_tx_fsr(int s) {
    u16 used = (u16)(get16(SOCK_BASE[s] + SR_TX_WR) - get16(SOCK_BASE[s] + SR_TX_RD));
    set16(SOCK_BASE[s] + SR_TX_FSR, (u16)(BUF_SIZE - used));
}

/* Install the post-reset hardware default values for the W5100S register
 * file. Per the W5100S datasheet § 3.2, several registers come up with
 * non-zero defaults (RTR, RCR, RMSR/TMSR, PHYCFGR, …). The maintainer of
 * the n4c-nettools driver reported (issue #123) that our previous code,
 * which preserved the common block across MR.RST, was contrary to the
 * datasheet — §3.1.1 says MR.RST initialises ALL registers. So both the
 * cold-reset path and the MR.RST path now wipe the file and then call
 * this helper to put the silicon defaults back. */
static void restore_hw_defaults(void) {
    /* Common block */
    regs[0x0000] = 0x03;   /* MR: BUS_SEL ties IND + AI on the Net4CPC board */
    regs[0x0017] = 0x07;   /* RTR  = 0x07D0 → 200 ms retry timeout         */
    regs[0x0018] = 0xD0;
    regs[0x0019] = 0x08;   /* RCR  = 8 retries                              */
    regs[0x001A] = 0x55;   /* RMSR = 2 KB per socket (01b × 4 sockets)      */
    regs[0x001B] = 0x55;   /* TMSR = 2 KB per socket                        */
    regs[0x0028] = 0x28;   /* per silicon                                   */
    regs[0x0030] = 0x40;
    regs[0x003A] = 0xFF;
    regs[0x003B] = 0xFF;
    /* PHYCFGR (0x003C): we report LNK=1, SPD=1, DPX=1 so KCNet utilities
     * (NCFG, PING, NTIME, …) pass their link-up gate — the host owns the
     * upstream interface, so the wire is always "up" from the CPC's
     * point of view. */
    regs[0x003C] = 0x07;
    regs[0x003D] = 0x81;
    regs[0x0045] = 0x01;
    regs[0x0047] = 0x41;
    regs[0x004D] = 0x07;
    regs[0x004E] = 0xD0;
    regs[0x0080] = 0x51;
    tcntr_reset();
}

static void close_sock(int s) {
    if (sock_fd[s] != -1) {
        sock_close(sock_fd[s]);
        sock_fd[s] = -1;
    }
}

/* ---------------------------------------------------------------------------
 * Receive polling
 * ------------------------------------------------------------------------- */

static void write_rx_byte(int s, u8 b) {
    regs[RX_BASE[s] + (rx_wr[s] & BUF_MASK)] = b;
    rx_wr[s]++;
}

static void poll_rx(int s) {
    if (sock_fd[s] == -1) return;

    u8 sr = regs[SOCK_BASE[s] + SR_SR];
    if (sr != SSTAT_ESTABLISHED && sr != SSTAT_UDP) return;

    u16 used  = (u16)(rx_wr[s] - get16(SOCK_BASE[s] + SR_RX_RD));
    u16 space = (u16)(BUF_SIZE - used);
    if (space == 0) return;

    u8 mode = regs[SOCK_BASE[s] + SR_MR] & 0x0F;

    if (mode == SMODE_UDP) {
        if (space <= 8) return;
        u16 payload_space = (u16)(space - 8);

        u8 tmp[2048];
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);
        memset(&src, 0, sizeof(src));
        ssize_t n = recvfrom(sock_fd[s], (char *)tmp,
                             payload_space < sizeof(tmp) ? payload_space : sizeof(tmp),
                             0, (struct sockaddr *)&src, &srclen);
        if (n <= 0) return;

        u32 src_ip   = ntohl(src.sin_addr.s_addr);
        u16 src_port = ntohs(src.sin_port);

        /* 8-byte W5100S UDP header */
        write_rx_byte(s, (u8)(src_ip >> 24));
        write_rx_byte(s, (u8)(src_ip >> 16));
        write_rx_byte(s, (u8)(src_ip >>  8));
        write_rx_byte(s, (u8)(src_ip));
        write_rx_byte(s, (u8)(src_port >> 8));
        write_rx_byte(s, (u8)(src_port));
        write_rx_byte(s, (u8)(n >> 8));
        write_rx_byte(s, (u8)(n));
        for (ssize_t i = 0; i < n; i++)
            write_rx_byte(s, tmp[i]);

    } else {
        u8 tmp[2048];
        ssize_t n = recv(sock_fd[s], (char *)tmp,
                         space < sizeof(tmp) ? space : sizeof(tmp), 0);
        if (n < 0) return;
        if (n == 0) {
            regs[SOCK_BASE[s] + SR_SR] = SSTAT_CLOSE_WAIT;
            close_sock(s);
            return;
        }
        for (ssize_t i = 0; i < n; i++)
            write_rx_byte(s, tmp[i]);
    }

    update_rx_rsr(s);
}

static void poll_connect(int s) {
    if (sock_fd[s] == -1) return;
    if (regs[SOCK_BASE[s] + SR_SR] != SSTAT_SYNSENT) return;

    fd_set wfds, efds;
    FD_ZERO(&wfds); FD_SET(sock_fd[s], &wfds);
    FD_ZERO(&efds); FD_SET(sock_fd[s], &efds);
    struct timeval tv = {0, 0};

    if (select(sock_fd[s] + 1, NULL, &wfds, &efds, &tv) > 0) {
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(sock_fd[s], SOL_SOCKET, SO_ERROR, (char *)&err, &len);
        if (err == 0) {
            regs[SOCK_BASE[s] + SR_SR] = SSTAT_ESTABLISHED;
            set16(SOCK_BASE[s] + SR_TX_FSR, BUF_SIZE);
        } else {
            regs[SOCK_BASE[s] + SR_SR] = SSTAT_CLOSED;
            close_sock(s);
        }
    }
}

/* ---------------------------------------------------------------------------
 * Socket command dispatch
 * ------------------------------------------------------------------------- */

static void handle_command(int s, u8 cmd) {
    u8 mode = regs[SOCK_BASE[s] + SR_MR] & 0x0F;

    switch (cmd) {

    case SCMD_OPEN:
        close_sock(s);
        /* TAP backend for TCP: tell the stack about the new socket and
         * reflect INIT immediately. UDP keeps using the POSIX backend
         * even with TAP because n4c_stack_send_udp() is selected per
         * call inside SCMD_SEND, so we still want the host socket open
         * to receive DHCP-style replies before SIPR is set. */
        if (mode == SMODE_TCP && n4c_stack_active()) {
            n4c_stack_tcp_open(s, get16(SOCK_BASE[s] + 0x04));
            regs[SOCK_BASE[s] + SR_SR] = SSTAT_INIT;
            set16(SOCK_BASE[s] + SR_TX_FSR, BUF_SIZE);
            set16(SOCK_BASE[s] + SR_TX_WR,  0);
            set16(SOCK_BASE[s] + SR_TX_RD,  0);
            set16(SOCK_BASE[s] + SR_RX_RSR, 0);
            set16(SOCK_BASE[s] + SR_RX_RD,  0);
            rx_wr[s] = 0;
            break;
        }
        if (mode == SMODE_UDP && n4c_stack_active()) {
            regs[SOCK_BASE[s] + SR_SR] = SSTAT_UDP;
            set16(SOCK_BASE[s] + SR_TX_FSR, BUF_SIZE);
            set16(SOCK_BASE[s] + SR_TX_WR,  0);
            set16(SOCK_BASE[s] + SR_TX_RD,  0);
            set16(SOCK_BASE[s] + SR_RX_RSR, 0);
            set16(SOCK_BASE[s] + SR_RX_RD,  0);
            rx_wr[s] = 0;
            break;
        }
        if (mode == SMODE_IPRAW && n4c_stack_active()) {
            regs[SOCK_BASE[s] + SR_SR] = SSTAT_IPRAW;
            set16(SOCK_BASE[s] + SR_TX_FSR, BUF_SIZE);
            set16(SOCK_BASE[s] + SR_TX_WR,  0);
            set16(SOCK_BASE[s] + SR_TX_RD,  0);
            set16(SOCK_BASE[s] + SR_RX_RSR, 0);
            set16(SOCK_BASE[s] + SR_RX_RD,  0);
            rx_wr[s] = 0;
            break;
        }
        sock_fd[s] = socket(AF_INET,
                            mode == SMODE_UDP ? SOCK_DGRAM : SOCK_STREAM, 0);
        if (sock_fd[s] != -1) {
            sock_set_nonblocking(sock_fd[s]);

            /* SO_REUSEADDR so two sockets can share the local port if
             * needed (the host may already have its own DHCP client on
             * port 68 etc. — at least don't error out instantly). */
            int one = 1;
            setsockopt(sock_fd[s], SOL_SOCKET, SO_REUSEADDR, (char *)&one, sizeof(one));

            /* SO_BROADCAST so UDP sendto(255.255.255.255) is permitted —
             * required for DHCP DISCOVER/REQUEST. */
            if (mode == SMODE_UDP)
                setsockopt(sock_fd[s], SOL_SOCKET, SO_BROADCAST, (char *)&one, sizeof(one));

            /* Bind to (SIPR, Sn_PORT). SymbOS configures Sn_PORT before
             * OPEN — DHCP client uses port 68, NTP uses an ephemeral
             * client port, etc. Binding to that port lets server replies
             * actually reach our socket. */
            u32 sipr = ((u32)regs[0x000F] << 24) | ((u32)regs[0x0010] << 16) |
                       ((u32)regs[0x0011] <<  8) |  (u32)regs[0x0012];
            u16 sport = get16(SOCK_BASE[s] + 0x04);   /* Sn_PORT */
            struct sockaddr_in src;
            memset(&src, 0, sizeof(src));
            src.sin_family      = AF_INET;
            src.sin_port        = htons(sport);
            src.sin_addr.s_addr = sipr ? htonl(sipr) : INADDR_ANY;
            /* Best-effort: if the host already owns this port, fall back
             * to a random one rather than killing the socket. */
            if (bind(sock_fd[s], (struct sockaddr *)&src, sizeof(src)) < 0 &&
                    sport != 0) {
                src.sin_port = 0;
                bind(sock_fd[s], (struct sockaddr *)&src, sizeof(src));
            }

            regs[SOCK_BASE[s] + SR_SR] = (mode == SMODE_TCP) ? SSTAT_INIT : SSTAT_UDP;
            set16(SOCK_BASE[s] + SR_TX_FSR, BUF_SIZE);
            set16(SOCK_BASE[s] + SR_TX_WR,  0);
            set16(SOCK_BASE[s] + SR_TX_RD,  0);
            set16(SOCK_BASE[s] + SR_RX_RSR, 0);
            set16(SOCK_BASE[s] + SR_RX_RD,  0);
            rx_wr[s] = 0;
        }
        break;

    case SCMD_LISTEN:
        if (mode == SMODE_TCP && n4c_stack_active()) {
            n4c_stack_tcp_listen(s);
            regs[SOCK_BASE[s] + SR_SR] = SSTAT_LISTEN;
        }
        break;

    case SCMD_CONNECT: {
        u8  ip0   = regs[SOCK_BASE[s] + SR_DIPR];
        u8  ip1   = regs[SOCK_BASE[s] + SR_DIPR + 1];
        u8  ip2   = regs[SOCK_BASE[s] + SR_DIPR + 2];
        u8  ip3   = regs[SOCK_BASE[s] + SR_DIPR + 3];
        u16 dport = get16(SOCK_BASE[s] + SR_DPORT);

        if (mode == SMODE_TCP && n4c_stack_active()) {
            u8 dst_ip[4] = { ip0, ip1, ip2, ip3 };
            n4c_stack_tcp_connect(s, dst_ip, dport);
            regs[SOCK_BASE[s] + SR_SR] = SSTAT_SYNSENT;
            break;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(dport);
        addr.sin_addr.s_addr = htonl(((u32)ip0 << 24) | ((u32)ip1 << 16) |
                                     ((u32)ip2 <<  8) |  (u32)ip3);

        int r = connect(sock_fd[s], (struct sockaddr *)&addr, sizeof(addr));
        if (r == 0 || sock_in_progress()) {
            regs[SOCK_BASE[s] + SR_SR] = SSTAT_SYNSENT;
        } else {
            regs[SOCK_BASE[s] + SR_SR] = SSTAT_CLOSED;
            close_sock(s);
        }
        break;
    }

    case SCMD_SEND: {
        u16 tx_rd = get16(SOCK_BASE[s] + SR_TX_RD);
        u16 tx_wr = get16(SOCK_BASE[s] + SR_TX_WR);
        u16 len   = (u16)(tx_wr - tx_rd);
        int udp_send_result = 1;  /* >0 means "send went out"; -1 means ARP failed */

        if (len > 0) {
            u8 buf[2048];
            for (u16 i = 0; i < len; i++)
                buf[i] = regs[TX_BASE[s] + ((tx_rd + i) & BUF_MASK)];

            if (mode == SMODE_UDP) {
                u8  dst_ip[4] = {
                    regs[SOCK_BASE[s] + SR_DIPR],
                    regs[SOCK_BASE[s] + SR_DIPR + 1],
                    regs[SOCK_BASE[s] + SR_DIPR + 2],
                    regs[SOCK_BASE[s] + SR_DIPR + 3],
                };
                u16 sport = get16(SOCK_BASE[s] + 0x04);
                u16 dport = get16(SOCK_BASE[s] + SR_DPORT);

                if (n4c_stack_active()) {
                    /* Real L2 path through TAP. */
                    int sent = n4c_stack_send_udp(sport, dst_ip, dport, buf, len);
                    /* Stash for the unified SEND_OK/TIMEOUT raise below. */
                    udp_send_result = sent;
                } else if (sock_fd[s] != -1) {
                    struct sockaddr_in dst;
                    memset(&dst, 0, sizeof(dst));
                    dst.sin_family = AF_INET;
                    dst.sin_port   = htons(dport);
                    dst.sin_addr.s_addr =
                        htonl(((u32)dst_ip[0] << 24) | ((u32)dst_ip[1] << 16) |
                              ((u32)dst_ip[2] <<  8) |  (u32)dst_ip[3]);
                    sendto(sock_fd[s], (const char *)buf, len, 0,
                           (struct sockaddr *)&dst, sizeof(dst));
                }
            } else if (mode == SMODE_IPRAW && n4c_stack_active()) {
                u8 dst_ip[4] = {
                    regs[SOCK_BASE[s] + SR_DIPR],
                    regs[SOCK_BASE[s] + SR_DIPR + 1],
                    regs[SOCK_BASE[s] + SR_DIPR + 2],
                    regs[SOCK_BASE[s] + SR_DIPR + 3],
                };
                u8 proto = regs[SOCK_BASE[s] + SR_PROTO];
                udp_send_result = n4c_stack_send_ip(proto, dst_ip, buf, len);
            } else if (mode == SMODE_TCP && n4c_stack_active()) {
                /* TAP-backed TCP. Don't advance TX_RD here — the stack
                 * does that via the ack callback when the peer ACKs. */
                int sent = n4c_stack_tcp_send(s, buf, (int)len);
                if (sent > 0) {
                    /* Leave TX_RD at tx_rd until SEND_OK; the kernel
                     * watches TX_FSR/RX_RSR. */
                    update_tx_fsr(s);
                    break;
                }
                /* Stack refused (not ESTABLISHED yet) — drop. */
            } else if (sock_fd[s] != -1) {
                send(sock_fd[s], (const char *)buf, len, 0);
            }
        }

        set16(SOCK_BASE[s] + SR_TX_RD, tx_wr);
        update_tx_fsr(s);
        /* W5100S signalling after SCMD_SEND on a UDP socket:
         *   SEND_OK (Sn_IR bit 4) — the chip successfully transmitted.
         *   TIMEOUT (Sn_IR bit 3) — the chip couldn't ARP-resolve the
         *     destination after its retry budget; the target IP appears
         *     to be unused on the wire.
         * NCFG uses TIMEOUT to mean "no ARP conflict — safe to claim
         * the address" during DHCP's RFC 5227 probe. So raising
         * SEND_OK on an ARP-failed unicast send is wrong — NCFG would
         * read it as "ARP succeeded → address in use" and send
         * DHCPDECLINE. (For broadcasts arp_resolve() returns the
         * broadcast MAC immediately so SEND_OK is always the right
         * answer there.) */
        if (mode == SMODE_UDP || mode == SMODE_IPRAW) {
            if (udp_send_result > 0)
                regs[SOCK_BASE[s] + SR_IR] |= 0x10;   /* SEND_OK */
            else
                regs[SOCK_BASE[s] + SR_IR] |= 0x08;   /* TIMEOUT */
        }
        break;
    }

    case SCMD_RECV:
        update_rx_rsr(s);
        poll_rx(s);
        break;

    case SCMD_DISCON:
        if (mode == SMODE_TCP && n4c_stack_active()) {
            n4c_stack_tcp_disconnect(s);
            break;
        }
        /* fall through to legacy close */
        /* fallthrough */
    case SCMD_CLOSE:
        if (mode == SMODE_TCP && n4c_stack_active()) {
            n4c_stack_tcp_close(s);
            regs[SOCK_BASE[s] + SR_SR]    = SSTAT_CLOSED;
            set16(SOCK_BASE[s] + SR_RX_RSR, 0);
            set16(SOCK_BASE[s] + SR_TX_FSR, 0);
            rx_wr[s] = 0;
            break;
        }
        close_sock(s);
        regs[SOCK_BASE[s] + SR_SR]    = SSTAT_CLOSED;
        set16(SOCK_BASE[s] + SR_RX_RSR, 0);
        set16(SOCK_BASE[s] + SR_TX_FSR, 0);
        rx_wr[s] = 0;
        break;

    default:
        break;
    }

    regs[SOCK_BASE[s] + SR_CR] = 0x00; /* command register self-clears */
}

/* ---------------------------------------------------------------------------
 * Trace helpers
 * ------------------------------------------------------------------------- */

/* Decode the W5100S internal address to a human-readable register name plus
 * an optional socket index. Returns a pointer to a static string. */
static const char *reg_name(u16 addr, int *sock_out) {
    *sock_out = -1;
    /* Common register block (0x0000-0x002F) */
    switch (addr) {
    case 0x0000: return "MR";
    case 0x0001: case 0x0002: case 0x0003: case 0x0004: return "GAR";
    case 0x0005: case 0x0006: case 0x0007: case 0x0008: return "SUBR";
    case 0x0009: case 0x000A: case 0x000B: case 0x000C: case 0x000D: case 0x000E: return "SHAR";
    case 0x000F: case 0x0010: case 0x0011: case 0x0012: return "SIPR";
    case 0x0015: return "IR";
    case 0x0016: return "IMR";
    case 0x0017: case 0x0018: return "RTR";
    case 0x0019: return "RCR";
    case 0x001A: return "RMSR";
    case 0x001B: return "TMSR";
    }
    /* Socket registers (0x0400-0x07FF) */
    for (int s = 0; s < 4; s++) {
        if (addr >= SOCK_BASE[s] && addr < SOCK_BASE[s] + 0x100) {
            *sock_out = s;
            u16 off = (u16)(addr - SOCK_BASE[s]);
            switch (off) {
            case 0x00: return "Sn_MR";
            case 0x01: return "Sn_CR";
            case 0x02: return "Sn_IR";
            case 0x03: return "Sn_SR";
            case 0x04: case 0x05: return "Sn_PORT";
            case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: return "Sn_DHAR";
            case 0x0C: case 0x0D: case 0x0E: case 0x0F: return "Sn_DIPR";
            case 0x10: case 0x11: return "Sn_DPORT";
            case 0x12: case 0x13: return "Sn_MSSR";
            case 0x14: return "Sn_PROTO";
            case 0x15: return "Sn_TOS";
            case 0x16: return "Sn_TTL";
            case 0x20: case 0x21: return "Sn_TX_FSR";
            case 0x22: case 0x23: return "Sn_TX_RD";
            case 0x24: case 0x25: return "Sn_TX_WR";
            case 0x26: case 0x27: return "Sn_RX_RSR";
            case 0x28: case 0x29: return "Sn_RX_RD";
            }
            return "Sn_?";
        }
    }
    /* TX / RX buffer regions */
    for (int s = 0; s < 4; s++) {
        if (addr >= TX_BASE[s] && addr < TX_BASE[s] + 0x800) { *sock_out = s; return "TX_buf"; }
        if (addr >= RX_BASE[s] && addr < RX_BASE[s] + 0x800) { *sock_out = s; return "RX_buf"; }
    }
    return "?";
}

static const char *scmd_name(u8 cmd) {
    switch (cmd) {
    case SCMD_OPEN:    return "OPEN";
    case SCMD_CONNECT: return "CONNECT";
    case SCMD_DISCON:  return "DISCON";
    case SCMD_CLOSE:   return "CLOSE";
    case SCMD_SEND:    return "SEND";
    case SCMD_RECV:    return "RECV";
    case 0x02:         return "LISTEN";
    default:           return "?";
    }
}

/* Coalesce TX/RX buffer accesses — log the start of each burst rather than
 * every byte. A "burst" is a run of consecutive addresses inside the same
 * buffer region. */
static u16 burst_first;
static int burst_len;
static char burst_kind;   /* 'R' = read, 'W' = write, 0 = idle */

static void burst_flush(void) {
    if (burst_kind && burst_len > 0)
        fprintf(stderr, "[net4cpc] burst %c [%04X..%04X] %d bytes\n",
                burst_kind, burst_first,
                (u16)(burst_first + burst_len - 1), burst_len);
    burst_kind = 0;
    burst_len = 0;
}

static bool is_buffer_addr(u16 addr) {
    return addr >= 0x4000 && addr < 0x8000;
}

static void trace_access(u16 addr, u8 val, bool is_write) {
    if (is_buffer_addr(addr)) {
        char kind = is_write ? 'W' : 'R';
        if (burst_kind == kind && burst_first + burst_len == addr)
            burst_len++;
        else {
            burst_flush();
            burst_kind = kind;
            burst_first = addr;
            burst_len = 1;
        }
        return;
    }
    burst_flush();
    int s; const char *n = reg_name(addr, &s);
    if (s >= 0) {
        if (is_write && (addr & 0xFF) == SR_CR)
            fprintf(stderr, "[net4cpc] S%d CMD=%02X (%s)\n", s, val, scmd_name(val));
        else
            fprintf(stderr, "[net4cpc] %s %04X %s/S%d = %02X\n",
                    is_write ? "W" : "R", addr, n, s, val);
    } else {
        fprintf(stderr, "[net4cpc] %s %04X %s = %02X\n",
                is_write ? "W" : "R", addr, n, val);
    }
}

/* ---------------------------------------------------------------------------
 * Register read/write with side-effects
 * ------------------------------------------------------------------------- */

static u8 reg_read(u16 addr) {
    /* W5100S indirect bus mode: KCNet utilities OR 0x8000 into every
     * address (per a Wiznet app note). The chip masks bit 15 internally.
     * Without this, every socket reg access lands in the unused upper
     * half of regs[] — writes look like they took, but the dispatcher
     * comparisons against SOCK_BASE[s] never match, so SCMD_* never
     * triggers handle_command. */
    addr &= 0x7FFF;
    u8 val;
    /* TCNTR (0x0082/0x0083): reading either byte latches the elapsed
     * 100 µs counter into the register pair so successive byte reads
     * see a coherent value. */
    if (addr == 0x0082 || addr == 0x0083) {
        set16(0x0082, tcntr_read());
    }
    for (int s = 0; s < 4; s++) {
        if (addr == SOCK_BASE[s] + SR_SR) {
            poll_connect(s);
            val = regs[addr];
            if (net4cpc_trace) trace_access(addr, val, false);
            return val;
        }
        /* Reading either byte of RX_RSR triggers a receive poll */
        if (addr == SOCK_BASE[s] + SR_RX_RSR ||
            addr == (u16)(SOCK_BASE[s] + SR_RX_RSR + 1)) {
            poll_rx(s);
            val = regs[addr];
            if (net4cpc_trace) trace_access(addr, val, false);
            return val;
        }
    }
    val = regs[addr];
    if (net4cpc_trace) trace_access(addr, val, false);
    return val;
}

static void reg_write(u16 addr, u8 val) {
    /* See reg_read() for the 0x8000-mask rationale. */
    addr &= 0x7FFF;
    if (net4cpc_trace) trace_access(addr, val, true);
    /* MR.RST (bit 7): software reset. Per W5100S datasheet § 3.1.1 a
     * soft reset initialises ALL W5100S registers — common block and
     * sockets alike — back to their hardware defaults (which for some
     * registers are non-zero; see restore_hw_defaults). Our previous
     * implementation preserved the common block, which was incorrect
     * (issue #123). */
    if (addr == 0x0000 && (val & 0x80)) {
        for (int s = 0; s < 4; s++) {
            close_sock(s);
            rx_wr[s] = 0;
        }
        memset(regs, 0, sizeof(regs));
        restore_hw_defaults();
        if (net4cpc_trace)
            fprintf(stderr, "[net4cpc]     soft reset complete; "
                            "all registers restored to hardware defaults\n");
        return;
    }
    /* TCNTCLR (0x0088): writing any value restarts the ticker. */
    if (addr == 0x0088) {
        tcntr_reset();
        return;
    }
    /* Sn_IR is write-1-to-clear per W5100S datasheet § 4.2.10. The
     * KCNet utilities (NCFG, PING, ...) write back the exact bits they
     * just observed in order to acknowledge them; storing val directly
     * would re-set the bits the kernel was trying to clear. We hit
     * this with DHCP: NCFG cleared SEND_OK+RECV after the OFFER, our
     * implementation stored those bits, the post-ACK probe then read
     * 0x1C (SEND_OK|RECV|TIMEOUT) and NCFG concluded the address was
     * in use → DHCPDECLINE. */
    bool is_sn_ir = false;
    for (int s = 0; s < 4; s++)
        if (addr == SOCK_BASE[s] + SR_IR) { is_sn_ir = true; break; }
    if (is_sn_ir)
        regs[addr] &= ~val;
    else
        regs[addr] = val;
    /* If the kernel just touched one of the common-config bytes
     * (SHAR, SIPR, GAR, SUBR), push the new values to the stack so
     * outgoing frames and ARP cache use the live config. */
    if (addr <= 0x12) stack_sync_config();
    for (int s = 0; s < 4; s++) {
        if (addr == SOCK_BASE[s] + SR_CR && val != 0) {
            handle_command(s, val);
            return;
        }
    }
}

/* ---------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

void net4cpc_reset(void) {
    memset(regs, 0, sizeof(regs));
    idm_ar = 0;
    for (int s = 0; s < 4; s++) {
        close_sock(s);
        rx_wr[s] = 0;
    }
    restore_hw_defaults();
}

int net4cpc_attach_tap(const char *devname) {
    if (tap_fd >= 0) {
        tap_close(tap_fd);
        tap_fd = -1;
        tap_dev_name[0] = '\0';
        n4c_stack_attach(-1, regs + 0x09, regs + 0x0F, regs + 0x01, regs + 0x05);
    }
    if (!devname) return 0;
    int fd = tap_open(devname, tap_dev_name, sizeof(tap_dev_name));
    if (fd < 0) return -1;
    tap_fd = fd;
    n4c_stack_attach(tap_fd, regs + 0x09, regs + 0x0F, regs + 0x01, regs + 0x05);
    n4c_stack_set_udp_deliver(stack_deliver_udp);
    n4c_stack_set_ip_deliver (stack_deliver_ip);
    n4c_stack_set_tcp_callbacks(stack_tcp_state,
                                stack_tcp_data,
                                stack_tcp_ack);
    fprintf(stderr, "net4cpc: TAP backend attached on '%s' (fd=%d)\n",
            tap_dev_name, tap_fd);
    return 0;
}

void net4cpc_poll(void) {
    if (tap_fd < 0) return;
    n4c_stack_poll();
}

static void stack_sync_config(void) {
    if (tap_fd < 0) return;
    n4c_stack_update_config(regs + 0x09, regs + 0x0F, regs + 0x01, regs + 0x05);
}

/* Called by the stack when an inbound UDP datagram is bound for one of
 * our local ports. Find a SOCK_UDP socket with Sn_PORT == dst_port and
 * push the payload into its RX buffer (with the 8-byte W5100S UDP
 * header). Returns 1 on accept, 0 on drop. */
/* TCP callbacks from the stack. The stack drives the wire side; these
 * three hooks mutate W5100S register state so the kernel observes the
 * lifecycle through Sn_SR + Sn_IR exactly as it would on real silicon. */
static void stack_tcp_state(int s, u8 new_sr) {
    if (s < 0 || s > 3) return;
    u8 old = regs[SOCK_BASE[s] + SR_SR];
    regs[SOCK_BASE[s] + SR_SR] = new_sr;
    /* Sn_IR bits per W5100S datasheet: CON=0x01, DISCON=0x02,
     * RECV=0x04, TIMEOUT=0x08, SEND_OK=0x10. */
    if (new_sr == 0x17 /* ESTABLISHED */ && old != 0x17)
        regs[SOCK_BASE[s] + SR_IR] |= 0x01;
    if (new_sr == 0x00 /* CLOSED */ && old != 0x00)
        regs[SOCK_BASE[s] + SR_IR] |= 0x02;
}
static int stack_tcp_data(int s, const u8 *data, int len) {
    if (s < 0 || s > 3 || len <= 0) return 0;
    u16 used  = (u16)(rx_wr[s] - get16(SOCK_BASE[s] + SR_RX_RD));
    u16 space = (u16)(BUF_SIZE - used);
    if (space == 0) return 0;
    int take = len;
    if (take > space) take = space;
    for (int i = 0; i < take; i++)
        write_rx_byte(s, data[i]);
    update_rx_rsr(s);
    regs[SOCK_BASE[s] + SR_IR] |= 0x04;   /* RECV */
    return take;
}
static void stack_tcp_ack(int s, u16 acked) {
    if (s < 0 || s > 3) return;
    u16 tx_rd = get16(SOCK_BASE[s] + SR_TX_RD);
    set16(SOCK_BASE[s] + SR_TX_RD, (u16)(tx_rd + acked));
    update_tx_fsr(s);
    regs[SOCK_BASE[s] + SR_IR] |= 0x10;   /* SEND_OK */
}

static int stack_deliver_udp(const u8 src_ip[4], u16 src_port,
                             u16 dst_port,
                             const u8 *payload, u16 payload_len) {
    for (int s = 0; s < 4; s++) {
        u8 mode = regs[SOCK_BASE[s] + SR_MR] & 0x0F;
        u8 sr   = regs[SOCK_BASE[s] + SR_SR];
        if (mode != SMODE_UDP || sr != SSTAT_UDP) continue;
        if (get16(SOCK_BASE[s] + 0x04) != dst_port) continue;

        /* Need 8 + payload_len bytes of buffer space */
        u16 used  = (u16)(rx_wr[s] - get16(SOCK_BASE[s] + SR_RX_RD));
        u16 space = (u16)(BUF_SIZE - used);
        if (space < 8u + payload_len) return 0;

        write_rx_byte(s, src_ip[0]);
        write_rx_byte(s, src_ip[1]);
        write_rx_byte(s, src_ip[2]);
        write_rx_byte(s, src_ip[3]);
        write_rx_byte(s, (u8)(src_port >> 8));
        write_rx_byte(s, (u8)(src_port));
        write_rx_byte(s, (u8)(payload_len >> 8));
        write_rx_byte(s, (u8)(payload_len));
        for (u16 i = 0; i < payload_len; i++)
            write_rx_byte(s, payload[i]);
        update_rx_rsr(s);
        regs[SOCK_BASE[s] + SR_IR] |= 0x04;   /* RECV */
        return 1;
    }
    return 0;
}

/* IPRAW inbound: find a socket in IPRAW mode whose Sn_PROTO matches
 * this frame's IP protocol, and push it the W5100S IPRAW frame format
 * (4 bytes src IP, 2 bytes length, then the L3 payload). */
static int stack_deliver_ip(u8 proto, const u8 src_ip[4],
                            const u8 *payload, u16 payload_len) {
    for (int s = 0; s < 4; s++) {
        u8 mode = regs[SOCK_BASE[s] + SR_MR] & 0x0F;
        u8 sr   = regs[SOCK_BASE[s] + SR_SR];
        if (mode != SMODE_IPRAW || sr != SSTAT_IPRAW) continue;
        if (regs[SOCK_BASE[s] + SR_PROTO] != proto) continue;

        u16 used  = (u16)(rx_wr[s] - get16(SOCK_BASE[s] + SR_RX_RD));
        u16 space = (u16)(BUF_SIZE - used);
        if (space < 6u + payload_len) return 0;

        write_rx_byte(s, src_ip[0]);
        write_rx_byte(s, src_ip[1]);
        write_rx_byte(s, src_ip[2]);
        write_rx_byte(s, src_ip[3]);
        write_rx_byte(s, (u8)(payload_len >> 8));
        write_rx_byte(s, (u8)(payload_len));
        for (u16 i = 0; i < payload_len; i++)
            write_rx_byte(s, payload[i]);
        update_rx_rsr(s);
        regs[SOCK_BASE[s] + SR_IR] |= 0x04;   /* RECV */
        return 1;
    }
    return 0;
}

u8 net4cpc_in(u8 reg_sel) {
    u8 val;
    switch (reg_sel & 0x03) {
    case 0:
        val = regs[0x0000];                /* MR direct shortcut at port 0xFD20 */
        if (net4cpc_trace)
            fprintf(stderr, "[net4cpc] IN  FD20 MR_shortcut = %02X\n", val);
        return val;
    case 1: return (u8)(idm_ar >> 8);      /* IDM_ARH */
    case 2: return (u8)(idm_ar & 0xFF);    /* IDM_ARL */
    case 3:                                /* IDM_DR */
        val = reg_read(idm_ar);
        if (regs[0x0000] & 0x02) idm_ar++;
        return val;
    default: return 0xFF;
    }
}

void net4cpc_out(u8 reg_sel, u8 val) {
    switch (reg_sel & 0x03) {
    case 0:                                /* MR (direct shortcut) */
        if (net4cpc_trace)
            fprintf(stderr, "[net4cpc] OUT FD20 MR = %02X\n", val);
        reg_write(0x0000, val);            /* shares MR.RST handling */
        break;
    case 1:                                /* IDM_ARH */
        idm_ar = (u16)((idm_ar & 0x00FFu) | ((u16)val << 8));
        break;
    case 2:                                /* IDM_ARL */
        idm_ar = (u16)((idm_ar & 0xFF00u) | val);
        break;
    case 3:                                /* IDM_DR */
        reg_write(idm_ar, val);
        if (regs[0x0000] & 0x02) idm_ar++;
        break;
    }
}

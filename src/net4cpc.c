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
#include "compat_win.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

int net4cpc_trace = 0;

/* TAP backend state. When tap_fd >= 0 we route Ethernet frames through it
 * instead of (or in addition to, during phase-in) the legacy host-POSIX-
 * socket fallback. Phases 1-5 of #104 add the actual frame plumbing; this
 * commit just wires the open/poll surface so the rest can land in steps. */
static int  tap_fd        = -1;
static char tap_dev_name[32] = { 0 };

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
#define SR_SR     0x03u
#define SR_DIPR   0x0Cu /* 4 bytes, big-endian */
#define SR_DPORT  0x10u /* 2 bytes, big-endian */
#define SR_TX_FSR 0x20u /* 2 bytes */
#define SR_TX_RD  0x22u /* 2 bytes */
#define SR_TX_WR  0x24u /* 2 bytes */
#define SR_RX_RSR 0x26u /* 2 bytes */
#define SR_RX_RD  0x28u /* 2 bytes */

/* Socket modes */
#define SMODE_TCP 0x01u
#define SMODE_UDP 0x02u

/* Socket status */
#define SSTAT_CLOSED      0x00u
#define SSTAT_INIT        0x13u
#define SSTAT_SYNSENT     0x15u
#define SSTAT_ESTABLISHED 0x17u
#define SSTAT_CLOSE_WAIT  0x1Cu
#define SSTAT_UDP         0x22u

/* Socket commands */
#define SCMD_OPEN    0x01u
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

    case SCMD_CONNECT: {
        u8  ip0   = regs[SOCK_BASE[s] + SR_DIPR];
        u8  ip1   = regs[SOCK_BASE[s] + SR_DIPR + 1];
        u8  ip2   = regs[SOCK_BASE[s] + SR_DIPR + 2];
        u8  ip3   = regs[SOCK_BASE[s] + SR_DIPR + 3];
        u16 dport = get16(SOCK_BASE[s] + SR_DPORT);

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

        if (len > 0 && sock_fd[s] != -1) {
            u8 buf[2048];
            for (u16 i = 0; i < len; i++)
                buf[i] = regs[TX_BASE[s] + ((tx_rd + i) & BUF_MASK)];

            if (mode == SMODE_UDP) {
                u8  ip0   = regs[SOCK_BASE[s] + SR_DIPR];
                u8  ip1   = regs[SOCK_BASE[s] + SR_DIPR + 1];
                u8  ip2   = regs[SOCK_BASE[s] + SR_DIPR + 2];
                u8  ip3   = regs[SOCK_BASE[s] + SR_DIPR + 3];
                u16 dport = get16(SOCK_BASE[s] + SR_DPORT);

                struct sockaddr_in dst;
                memset(&dst, 0, sizeof(dst));
                dst.sin_family = AF_INET;
                dst.sin_port   = htons(dport);
                dst.sin_addr.s_addr = htonl(((u32)ip0 << 24) | ((u32)ip1 << 16) |
                                            ((u32)ip2 <<  8) |  (u32)ip3);
                sendto(sock_fd[s], (const char *)buf, len, 0,
                       (struct sockaddr *)&dst, sizeof(dst));
            } else {
                send(sock_fd[s], (const char *)buf, len, 0);
            }
        }

        set16(SOCK_BASE[s] + SR_TX_RD, tx_wr);
        update_tx_fsr(s);
        break;
    }

    case SCMD_RECV:
        update_rx_rsr(s);
        poll_rx(s);
        break;

    case SCMD_DISCON:
    case SCMD_CLOSE:
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
    u8 val;
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
    if (net4cpc_trace) trace_access(addr, val, true);
    /* MR.RST (bit 7): software reset. Real silicon clears all regs and
     * the RST bit auto-clears within ~10us. The Net4CPC board ties the
     * BUS_SEL pin to indirect-bus mode, so MR comes back as 0x03 (IND +
     * AI). Clearing MR to 0x00 here disables auto-increment, which
     * breaks every multi-byte register write SymbOS makes afterward. */
    if (addr == 0x0000 && (val & 0x80)) {
        for (int s = 0; s < 4; s++) {
            close_sock(s);
            rx_wr[s] = 0;
        }
        memset(regs, 0, sizeof(regs));
        regs[0x0000] = 0x03;
        if (net4cpc_trace)
            fprintf(stderr, "[net4cpc]     soft reset complete; MR -> 03 (IND+AI)\n");
        return;
    }
    regs[addr] = val;
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
    /* MR = 0x03: indirect bus mode + auto-increment.
     * The n4c-nettools driver reads this port to confirm the chip is present. */
    regs[0x0000] = 0x03;
}

int net4cpc_attach_tap(const char *devname) {
    if (tap_fd >= 0) {
        tap_close(tap_fd);
        tap_fd = -1;
        tap_dev_name[0] = '\0';
    }
    if (!devname) return 0;
    int fd = tap_open(devname, tap_dev_name, sizeof(tap_dev_name));
    if (fd < 0) return -1;
    tap_fd = fd;
    fprintf(stderr, "net4cpc: TAP backend attached on '%s' (fd=%d)\n",
            tap_dev_name, tap_fd);
    return 0;
}

void net4cpc_poll(void) {
    if (tap_fd < 0) return;
    /* Drain whatever the TAP has queued and dispatch. Phases 2-5 of #104
     * implement the actual frame parsing (ARP, IP, UDP, ICMP, TCP). For
     * now we just clear the queue so it doesn't back up; once those land
     * the frames will be routed to the right W5100S sockets. */
    u8 frame[TAP_FRAME_MAX];
    int drained = 0;
    while (1) {
        int n = tap_read(tap_fd, frame, sizeof(frame));
        if (n <= 0) break;
        drained++;
        if (drained > 64) break;     /* don't starve the Z80 on a flood */
    }
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

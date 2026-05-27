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

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

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
        close(sock_fd[s]);
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
            int flags = fcntl(sock_fd[s], F_GETFL, 0);
            fcntl(sock_fd[s], F_SETFL, flags | O_NONBLOCK);

            /* Bind to the source IP the CPC configured in SIPR (0x000F–0x0012) */
            u32 sipr = ((u32)regs[0x000F] << 24) | ((u32)regs[0x0010] << 16) |
                       ((u32)regs[0x0011] <<  8) |  (u32)regs[0x0012];
            if (sipr != 0) {
                struct sockaddr_in src;
                memset(&src, 0, sizeof(src));
                src.sin_family      = AF_INET;
                src.sin_port        = 0;
                src.sin_addr.s_addr = htonl(sipr);
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
        if (r == 0 || errno == EINPROGRESS) {
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
 * Register read/write with side-effects
 * ------------------------------------------------------------------------- */

static u8 reg_read(u16 addr) {
    for (int s = 0; s < 4; s++) {
        if (addr == SOCK_BASE[s] + SR_SR) {
            poll_connect(s);
            return regs[addr];
        }
        /* Reading either byte of RX_RSR triggers a receive poll */
        if (addr == SOCK_BASE[s] + SR_RX_RSR ||
            addr == (u16)(SOCK_BASE[s] + SR_RX_RSR + 1)) {
            poll_rx(s);
            return regs[addr];
        }
    }
    return regs[addr];
}

static void reg_write(u16 addr, u8 val) {
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

u8 net4cpc_in(u8 reg_sel) {
    switch (reg_sel & 0x03) {
    case 0: return regs[0x0000];           /* MR direct shortcut */
    case 1: return (u8)(idm_ar >> 8);      /* IDM_ARH */
    case 2: return (u8)(idm_ar & 0xFF);    /* IDM_ARL */
    case 3: {                              /* IDM_DR */
        u8 val = reg_read(idm_ar);
        if (regs[0x0000] & 0x02) idm_ar++;
        return val;
    }
    default: return 0xFF;
    }
}

void net4cpc_out(u8 reg_sel, u8 val) {
    switch (reg_sel & 0x03) {
    case 0:                                /* MR */
        regs[0x0000] = val;
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

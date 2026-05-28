#define _GNU_SOURCE
#include "symbnet.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>

/* ---- Opcodes (see protocol README) ---- */
#define OP_TCP_OPEN_CLIENT  0x01
#define OP_TCP_OPEN_SERVER  0x02
#define OP_TCP_CLOSE        0x03
#define OP_TCP_STATUS       0x04
#define OP_TCP_RECV         0x05
#define OP_TCP_SEND         0x06
#define OP_TCP_DISCONNECT   0x07
#define OP_UDP_OPEN         0x11
#define OP_UDP_CLOSE        0x13
#define OP_UDP_STATUS       0x14
#define OP_UDP_RECV         0x15
#define OP_UDP_SEND         0x16
#define OP_GET_CONFIG       0x20
#define OP_RESOLVE          0x21
#define OP_RESET            0x2F

/* State codes */
#define ST_UNUSED   0
#define ST_LISTEN   1
#define ST_ESTAB    2
#define ST_CLWAIT   3
#define ST_CLOSED   4

/* Error codes */
#define ERR_OK      0x00
#define ERR_BADSOCK 0x01
#define ERR_INUSE   0x02
#define ERR_REFUSED 0x03
#define ERR_RESET   0x04
#define ERR_TIMEOUT 0x05
#define ERR_DNS     0x06
#define ERR_BUFFULL 0x07
#define ERR_INTERN  0xFF

/* ---- Helpers ---- */

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static void close_sock(SymbNetSock *s) {
    if (s->fd >= 0) close(s->fd);
    memset(s, 0, sizeof(*s));
    s->fd = -1;
}

/* Append `n` bytes to the response buffer, growing the length counter. */
static void resp_put(SymbNet *n, const void *p, int len) {
    if (n->resp_len + len > SYMBNET_BUFLEN) return;
    memcpy(&n->resp[n->resp_len], p, len);
    n->resp_len += len;
}
static void resp_u8(SymbNet *n, u8 v)  { resp_put(n, &v, 1); }
static void resp_u16(SymbNet *n, u16 v){ u8 b[2] = { v & 0xFF, v >> 8 }; resp_put(n, b, 2); }

/* Begin a response: reserves 2 bytes for length, returns offset of status byte. */
static int resp_begin(SymbNet *n) {
    n->resp_len = 0;
    n->resp_pos = 0;
    n->resp[0] = 0; n->resp[1] = 0;
    n->resp_len = 2;
    return 2;
}
/* Finalize: write length field (bytes after the 2-byte header). */
static void resp_finish(SymbNet *n) {
    int payload = n->resp_len - 2;
    n->resp[0] = payload & 0xFF;
    n->resp[1] = (payload >> 8) & 0xFF;
    /* Error flag is bit 1 of the FD31 status; we set it when the payload's
     * first byte (the status field) is non-zero. */
    n->last_error = (payload >= 1 && n->resp[2] != 0);
}

static SymbNetSock *sock_of(SymbNet *n, u8 sid) {
    if (sid >= SYMBNET_NSOCKS) return NULL;
    return &n->sockets[sid];
}

/* ---- Command handlers ---- */

static u8 do_tcp_open_client(SymbNet *n, u8 sid, u16 lport, const u8 *rip, u16 rport) {
    SymbNetSock *s = sock_of(n, sid);
    if (!s || s->fd >= 0) return ERR_BADSOCK;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return ERR_INTERN;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    set_nonblock(fd);
    if (lport) {
        struct sockaddr_in la = {0};
        la.sin_family = AF_INET;
        la.sin_port = htons(lport);
        bind(fd, (struct sockaddr *)&la, sizeof(la));
    }
    struct sockaddr_in ra = {0};
    ra.sin_family = AF_INET;
    memcpy(&ra.sin_addr.s_addr, rip, 4);
    ra.sin_port = htons(rport);
    int r = connect(fd, (struct sockaddr *)&ra, sizeof(ra));
    if (r != 0 && errno == EINPROGRESS) {
        /* Block up to 5 s for completion — matches the M4 driver fix. */
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        int pr = poll(&pfd, 1, 5000);
        if (pr <= 0) { close(fd); return ERR_TIMEOUT; }
        int soerr = 0; socklen_t l = sizeof(soerr);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &l);
        if (soerr == ECONNREFUSED) { close(fd); return ERR_REFUSED; }
        if (soerr)                 { close(fd); return ERR_INTERN; }
    } else if (r != 0) {
        close(fd);
        return (errno == ECONNREFUSED) ? ERR_REFUSED : ERR_INTERN;
    }
    s->fd          = fd;
    s->state       = ST_ESTAB;
    s->local_port  = lport;
    memcpy(s->remote_ip, rip, 4);
    s->remote_port = rport;
    s->is_udp      = false;
    return ERR_OK;
}

static u8 do_tcp_open_server(SymbNet *n, u8 sid, u16 lport) {
    SymbNetSock *s = sock_of(n, sid);
    if (!s || s->fd >= 0) return ERR_BADSOCK;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return ERR_INTERN;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    set_nonblock(fd);
    struct sockaddr_in la = {0};
    la.sin_family = AF_INET;
    la.sin_port   = htons(lport);
    if (bind(fd, (struct sockaddr *)&la, sizeof(la)) < 0 ||
        listen(fd, 1) < 0) {
        close(fd);
        return ERR_INUSE;
    }
    s->fd         = fd;
    s->state      = ST_LISTEN;
    s->local_port = lport;
    s->is_udp     = false;
    return ERR_OK;
}

static u8 do_tcp_status(SymbNet *n, u8 sid, u8 *state_out, u16 *avail_out) {
    SymbNetSock *s = sock_of(n, sid);
    if (!s || s->fd < 0) { *state_out = ST_UNUSED; *avail_out = 0; return ERR_BADSOCK; }
    *state_out = s->state;
    int avail = 0;
    if (s->state == ST_ESTAB || s->state == ST_CLWAIT) {
        if (ioctl(s->fd, FIONREAD, &avail) != 0) avail = 0;
        if (avail == 0) {
            /* Probe for peer close */
            char b;
            ssize_t r = recv(s->fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
            if (r == 0) {
                s->state = ST_CLOSED;
                *state_out = ST_CLOSED;
            }
        }
    }
    if (avail > 0xFFFF) avail = 0xFFFF;
    *avail_out = (u16)avail;
    return ERR_OK;
}

static u8 do_tcp_recv(SymbNet *n, u8 sid, u16 maxlen, u8 *out, u16 *got) {
    *got = 0;
    SymbNetSock *s = sock_of(n, sid);
    if (!s || s->fd < 0) return ERR_BADSOCK;
    if (maxlen > SYMBNET_BUFLEN - 16) maxlen = SYMBNET_BUFLEN - 16;
    ssize_t r = recv(s->fd, out, maxlen, MSG_DONTWAIT);
    if (r > 0)              { *got = (u16)r; return ERR_OK; }
    if (r == 0)             { s->state = ST_CLOSED; return ERR_OK; }
    if (errno == EAGAIN ||
        errno == EWOULDBLOCK) return ERR_OK;
    if (errno == ECONNRESET){ s->state = ST_CLOSED; return ERR_RESET; }
    return ERR_INTERN;
}

static u8 do_tcp_send(SymbNet *n, u8 sid, const u8 *data, u16 len, u16 *sent) {
    *sent = 0;
    SymbNetSock *s = sock_of(n, sid);
    if (!s || s->fd < 0) return ERR_BADSOCK;
    ssize_t r = send(s->fd, data, len, MSG_NOSIGNAL);
    if (r >= 0)             { *sent = (u16)r; return ERR_OK; }
    if (errno == EAGAIN ||
        errno == EWOULDBLOCK) return ERR_BUFFULL;
    if (errno == EPIPE ||
        errno == ECONNRESET){ s->state = ST_CLOSED; return ERR_RESET; }
    return ERR_INTERN;
}

static u8 do_close(SymbNet *n, u8 sid) {
    SymbNetSock *s = sock_of(n, sid);
    if (!s) return ERR_BADSOCK;
    close_sock(s);
    return ERR_OK;
}

static u8 do_udp_open(SymbNet *n, u8 sid, u16 lport) {
    SymbNetSock *s = sock_of(n, sid);
    if (!s || s->fd >= 0) return ERR_BADSOCK;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return ERR_INTERN;
    set_nonblock(fd);
    if (lport) {
        struct sockaddr_in la = {0};
        la.sin_family = AF_INET;
        la.sin_port   = htons(lport);
        if (bind(fd, (struct sockaddr *)&la, sizeof(la)) < 0) {
            close(fd);
            return ERR_INUSE;
        }
    }
    s->fd         = fd;
    s->state      = ST_ESTAB;        /* UDP "established" = open */
    s->is_udp     = true;
    s->local_port = lport;
    return ERR_OK;
}

static u8 do_udp_recv(SymbNet *n, u8 sid, u16 maxlen, u8 *out, u16 *got,
                      u8 ip_out[4], u16 *port_out) {
    *got = 0; *port_out = 0; memset(ip_out, 0, 4);
    SymbNetSock *s = sock_of(n, sid);
    if (!s || s->fd < 0 || !s->is_udp) return ERR_BADSOCK;
    if (maxlen > SYMBNET_BUFLEN - 32) maxlen = SYMBNET_BUFLEN - 32;
    struct sockaddr_in ra = {0};
    socklen_t rl = sizeof(ra);
    ssize_t r = recvfrom(s->fd, out, maxlen, MSG_DONTWAIT,
                         (struct sockaddr *)&ra, &rl);
    if (r >= 0) {
        *got = (u16)r;
        memcpy(ip_out, &ra.sin_addr.s_addr, 4);
        *port_out = ntohs(ra.sin_port);
        return ERR_OK;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) return ERR_OK;
    return ERR_INTERN;
}

static u8 do_udp_send(SymbNet *n, u8 sid, const u8 *rip, u16 rport,
                      const u8 *data, u16 len, u16 *sent) {
    *sent = 0;
    SymbNetSock *s = sock_of(n, sid);
    if (!s || s->fd < 0 || !s->is_udp) return ERR_BADSOCK;
    struct sockaddr_in ra = {0};
    ra.sin_family = AF_INET;
    memcpy(&ra.sin_addr.s_addr, rip, 4);
    ra.sin_port   = htons(rport);
    ssize_t r = sendto(s->fd, data, len, MSG_NOSIGNAL,
                       (struct sockaddr *)&ra, sizeof(ra));
    if (r >= 0)             { *sent = (u16)r; return ERR_OK; }
    if (errno == EAGAIN ||
        errno == EWOULDBLOCK) return ERR_BUFFULL;
    return ERR_INTERN;
}

static u8 do_resolve(const char *host, u8 ip_out[4]) {
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return ERR_DNS;
    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
    memcpy(ip_out, &sa->sin_addr.s_addr, 4);
    freeaddrinfo(res);
    return ERR_OK;
}

/* ---- Command dispatcher ---- */

static void execute(SymbNet *n) {
    const u8 *p = n->cmd;
    int plen = n->cmd_len;
    if (plen < 1) { resp_begin(n); resp_u8(n, ERR_INTERN); resp_finish(n); return; }
    u8 op = p[0];
    resp_begin(n);

    switch (op) {
    case OP_TCP_OPEN_CLIENT: {
        if (plen < 10) { resp_u8(n, ERR_INTERN); break; }
        u8 sid    = p[1];
        u16 lport = p[2] | (p[3] << 8);
        u16 rport = p[8] | (p[9] << 8);
        resp_u8(n, do_tcp_open_client(n, sid, lport, &p[4], rport));
        break;
    }
    case OP_TCP_OPEN_SERVER: {
        if (plen < 4) { resp_u8(n, ERR_INTERN); break; }
        u8 sid    = p[1];
        u16 lport = p[2] | (p[3] << 8);
        resp_u8(n, do_tcp_open_server(n, sid, lport));
        break;
    }
    case OP_TCP_CLOSE:
    case OP_TCP_DISCONNECT: {
        if (plen < 2) { resp_u8(n, ERR_INTERN); break; }
        resp_u8(n, do_close(n, p[1]));
        break;
    }
    case OP_TCP_STATUS: {
        if (plen < 2) { resp_u8(n, ERR_INTERN); break; }
        u8 state; u16 avail;
        u8 rc = do_tcp_status(n, p[1], &state, &avail);
        resp_u8(n, rc); resp_u8(n, state); resp_u16(n, avail);
        break;
    }
    case OP_TCP_RECV: {
        if (plen < 4) { resp_u8(n, ERR_INTERN); break; }
        u8 sid = p[1];
        u16 ml = p[2] | (p[3] << 8);
        u8 buf[SYMBNET_BUFLEN]; u16 got;
        u8 rc = do_tcp_recv(n, sid, ml, buf, &got);
        resp_u8(n, rc); resp_u16(n, got);
        if (got) resp_put(n, buf, got);
        break;
    }
    case OP_TCP_SEND: {
        if (plen < 4) { resp_u8(n, ERR_INTERN); break; }
        u8 sid = p[1];
        u16 dl = p[2] | (p[3] << 8);
        if (plen < 4 + dl) { resp_u8(n, ERR_INTERN); break; }
        u16 sent;
        u8 rc = do_tcp_send(n, sid, &p[4], dl, &sent);
        resp_u8(n, rc); resp_u16(n, sent);
        break;
    }
    case OP_UDP_OPEN: {
        if (plen < 4) { resp_u8(n, ERR_INTERN); break; }
        u8 sid    = p[1];
        u16 lport = p[2] | (p[3] << 8);
        resp_u8(n, do_udp_open(n, sid, lport));
        break;
    }
    case OP_UDP_CLOSE: {
        if (plen < 2) { resp_u8(n, ERR_INTERN); break; }
        resp_u8(n, do_close(n, p[1]));
        break;
    }
    case OP_UDP_STATUS: {
        if (plen < 2) { resp_u8(n, ERR_INTERN); break; }
        SymbNetSock *s = sock_of(n, p[1]);
        if (!s || s->fd < 0) { resp_u8(n, ERR_BADSOCK); resp_u16(n, 0); break; }
        int avail = 0;
        ioctl(s->fd, FIONREAD, &avail);
        if (avail > 0xFFFF) avail = 0xFFFF;
        resp_u8(n, ERR_OK); resp_u16(n, (u16)avail);
        break;
    }
    case OP_UDP_RECV: {
        if (plen < 4) { resp_u8(n, ERR_INTERN); break; }
        u8 sid = p[1];
        u16 ml = p[2] | (p[3] << 8);
        u8 buf[SYMBNET_BUFLEN]; u16 got; u8 ip[4]; u16 rport;
        u8 rc = do_udp_recv(n, sid, ml, buf, &got, ip, &rport);
        resp_u8(n, rc); resp_put(n, ip, 4); resp_u16(n, rport);
        resp_u16(n, got);
        if (got) resp_put(n, buf, got);
        break;
    }
    case OP_UDP_SEND: {
        if (plen < 10) { resp_u8(n, ERR_INTERN); break; }
        u8 sid    = p[1];
        u16 rport = p[6] | (p[7] << 8);
        u16 dl    = p[8] | (p[9] << 8);
        if (plen < 10 + dl) { resp_u8(n, ERR_INTERN); break; }
        u16 sent;
        u8 rc = do_udp_send(n, sid, &p[2], rport, &p[10], dl, &sent);
        resp_u8(n, rc); resp_u16(n, sent);
        break;
    }
    case OP_GET_CONFIG: {
        /* Stub values that match what M4 currently advertises; the host's
         * real address isn't visible to the guest and SymbOS only uses these
         * to populate its display. */
        static const u8 cfg[20] = {
            192,168,1,100,  255,255,255,0,  192,168,1,1,  8,8,8,8,  8,8,4,4
        };
        resp_put(n, cfg, 20);
        break;
    }
    case OP_RESOLVE: {
        if (plen < 2) { resp_u8(n, ERR_INTERN); break; }
        u8 nl = p[1];
        if (plen < 2 + nl) { resp_u8(n, ERR_INTERN); break; }
        char host[256];
        memcpy(host, &p[2], nl); host[nl] = 0;
        u8 ip[4] = {0};
        u8 rc = do_resolve(host, ip);
        resp_u8(n, rc); resp_put(n, ip, 4);
        break;
    }
    case OP_RESET:
        for (int i = 0; i < SYMBNET_NSOCKS; i++) close_sock(&n->sockets[i]);
        /* No response payload for RESET. Length will be 0. */
        break;
    default:
        resp_u8(n, ERR_INTERN);
        break;
    }
    resp_finish(n);
}

/* ---- Public API ---- */

void symbnet_init(SymbNet *n) {
    memset(n, 0, sizeof(*n));
    for (int i = 0; i < SYMBNET_NSOCKS; i++) n->sockets[i].fd = -1;
}

void symbnet_reset(SymbNet *n) {
    for (int i = 0; i < SYMBNET_NSOCKS; i++) close_sock(&n->sockets[i]);
    n->cmd_len = 0;
    n->cmd_expected = 0;
    n->resp_len = 0;
    n->resp_pos = 0;
    n->last_error = false;
}

void symbnet_tick(SymbNet *n) {
    /* Accept pending connections on listening TCP sockets so a server-mode
     * open() can transition to ESTAB without the CPU having to drive it. */
    for (int i = 0; i < SYMBNET_NSOCKS; i++) {
        SymbNetSock *s = &n->sockets[i];
        if (s->fd < 0 || s->state != ST_LISTEN) continue;
        struct sockaddr_in ra = {0};
        socklen_t rl = sizeof(ra);
        int cfd = accept(s->fd, (struct sockaddr *)&ra, &rl);
        if (cfd < 0) continue;
        set_nonblock(cfd);
        close(s->fd);
        s->fd = cfd;
        s->state = ST_ESTAB;
        memcpy(s->remote_ip, &ra.sin_addr.s_addr, 4);
        s->remote_port = ntohs(ra.sin_port);
    }
}

u8 symbnet_port_read(SymbNet *n, u8 port_lo) {
    if (port_lo == 0x30) {
        if (n->resp_pos < n->resp_len)
            return n->resp[n->resp_pos++];
        return 0;
    }
    if (port_lo == 0x31) {
        u8 s = 0;
        if (n->resp_pos < n->resp_len) s |= 0x01;
        if (n->last_error)             s |= 0x02;
        return s;
    }
    return 0xFF;
}

void symbnet_port_write(SymbNet *n, u8 port_lo, u8 val) {
    if (port_lo != 0x30) return;
    if (n->cmd_len < SYMBNET_BUFLEN)
        n->cmd[n->cmd_len++] = val;
    /* Length header arrives in the first 2 bytes; once we have them, decode
     * how many more we expect. */
    if (n->cmd_len == 2) {
        n->cmd_expected = n->cmd[0] | (n->cmd[1] << 8);
        if (n->cmd_expected < 1 || n->cmd_expected > SYMBNET_BUFLEN - 2) {
            /* Bad framing — drop and signal error on next status read. */
            n->cmd_len = 0;
            n->cmd_expected = 0;
            resp_begin(n);
            resp_u8(n, ERR_INTERN);
            resp_finish(n);
            return;
        }
    }
    if (n->cmd_expected > 0 && n->cmd_len == 2 + n->cmd_expected) {
        /* Shift the payload to the start of the buffer and execute. */
        memmove(n->cmd, &n->cmd[2], n->cmd_expected);
        n->cmd_len = n->cmd_expected;
        execute(n);
        n->cmd_len = 0;
        n->cmd_expected = 0;
    }
}

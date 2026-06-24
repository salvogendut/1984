/* perryfi.c — software AT modem. See perryfi.h.
 * Ported from 1985/src/perryfi.c; the only intentional divergences
 * are the AT&I identification banner and the docstring. */

#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define __BSD_VISIBLE 1
#endif

#include "perryfi.h"

#include <SDL3/SDL.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/* MSG_NOSIGNAL is a Linux extension; on the BSDs it's not defined.
 * SIGPIPE is ignored process-wide elsewhere so passing 0 here is
 * safe. Fallback AFTER <sys/socket.h> so the system header has had
 * its say first. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
#endif

#define MASK (PERRYFI_RING - 1)

/* ---------------------------------------------------------------- */
/* Ring buffer (modem → guest)                                      */

static inline size_t rb_count(size_t h, size_t t) { return (h - t) & MASK; }
static inline size_t rb_space(size_t h, size_t t) { return MASK - rb_count(h, t); }
static inline bool   rb_empty(size_t h, size_t t) { return h == t; }

static void rx_push(Perryfi *p, u8 b) {
    if (rb_space(p->rx_head, p->rx_tail) == 0) return;
    p->rx_buf[p->rx_head & MASK] = b;
    p->rx_head = (p->rx_head + 1) & MASK;
}
bool perryfi_rx_pop(Perryfi *p, u8 *out) {
    if (rb_empty(p->rx_head, p->rx_tail)) return false;
    *out = p->rx_buf[p->rx_tail & MASK];
    p->rx_tail = (p->rx_tail + 1) & MASK;
    return true;
}
bool perryfi_rx_has(const Perryfi *p) {
    return !rb_empty(p->rx_head, p->rx_tail);
}

static void emit_str(Perryfi *p, const char *s) {
    while (*s) rx_push(p, (u8)*s++);
}

/* Hayes result-code shipping. Verbose: "\r\nOK\r\n"; numeric: "0\r\n". */
typedef enum { RC_OK, RC_CONNECT, RC_RING, RC_NO_CARRIER, RC_ERROR, RC_NO_ANSWER } Result;
static void send_result(Perryfi *p, Result r) {
    if (p->quiet) return;
    rx_push(p, '\r'); rx_push(p, '\n');
    if (!p->verbose) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%d", (int)r);
        emit_str(p, buf);
    } else switch (r) {
        case RC_OK:         emit_str(p, "OK"); break;
        case RC_CONNECT:    emit_str(p, "CONNECT");
                           if (p->extended_codes) emit_str(p, " 9600");
                           break;
        case RC_RING:       emit_str(p, "RING"); break;
        case RC_NO_CARRIER: emit_str(p, "NO CARRIER"); break;
        case RC_ERROR:      emit_str(p, "ERROR"); break;
        case RC_NO_ANSWER:  emit_str(p, "NO ANSWER"); break;
    }
    rx_push(p, '\r'); rx_push(p, '\n');
}

/* ---------------------------------------------------------------- */
/* TCP client                                                       */

static void tcp_close(Perryfi *p) {
#ifndef _WIN32
    if (p->tcp_fd >= 0) { close(p->tcp_fd); p->tcp_fd = -1; }
#endif
    p->remote_host[0] = 0;
    p->remote_port    = 0;
}

#ifndef _WIN32
static int tcp_dial(Perryfi *p, const char *host, int port) {
    char portstr[8]; snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) {
        fprintf(stderr, "perryfi: getaddrinfo('%s'): %s\n", host, strerror(errno));
        return -1;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        fprintf(stderr, "perryfi: socket: %s\n", strerror(errno));
        freeaddrinfo(res); return -1;
    }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        fprintf(stderr, "perryfi: connect('%s:%d'): %s\n", host, port, strerror(errno));
        close(fd); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    p->tcp_fd = fd;
    snprintf(p->remote_host, sizeof(p->remote_host), "%s", host);
    p->remote_port = port;
    fprintf(stderr, "perryfi: connected to %s:%d\n", host, port);
    return 0;
}
#endif

/* ---------------------------------------------------------------- */
/* AT-command parser                                                */

static char *skip_ws(char *s) { while (*s == ' ' || *s == '\t') s++; return s; }
static char  upper(char c)    { return (c >= 'a' && c <= 'z') ? (c - 32) : c; }

/* Parse and execute one AT command line (without the leading "AT" or
 * the trailing CR). `cmd` is uppercased in place. */
static void exec_at(Perryfi *p, char *cmd) {
    char *s = cmd;
    if (!*s) { send_result(p, RC_OK); return; }

    char c = *s++;
    switch (c) {
        case 'E': p->echo    = (*s != '0'); send_result(p, RC_OK); return;
        case 'Q': p->quiet   = (*s == '1'); send_result(p, RC_OK); return;
        case 'V': p->verbose = (*s != '0'); send_result(p, RC_OK); return;
        case 'Z':
            p->echo = true; p->quiet = false; p->verbose = true;
            send_result(p, RC_OK); return;
        case 'I':
            emit_str(p, "\r\n1984 PerryFi AT-modem\r\n");
            send_result(p, RC_OK); return;
        case 'O':
            if (p->tcp_fd >= 0) {
                p->state = PERRYFI_STATE_ONLINE;
                send_result(p, RC_CONNECT);
            } else {
                send_result(p, RC_NO_CARRIER);
            }
            return;
        case 'H':
            tcp_close(p);
            p->state = PERRYFI_STATE_CMD;
            send_result(p, RC_OK); return;
        case 'A':
            send_result(p, RC_NO_CARRIER); return;
        case 'D': {
            s = skip_ws(s);
            if (*s == 'T' || *s == 'P') s++;
            s = skip_ws(s);
            char host[128] = {0}; int port = 23;
            char *colon = strrchr(s, ':');
            if (colon) {
                size_t hl = (size_t)(colon - s);
                if (hl >= sizeof(host)) hl = sizeof(host) - 1;
                memcpy(host, s, hl);
                port = atoi(colon + 1);
            } else {
                snprintf(host, sizeof(host), "%s", s);
            }
            if (!host[0] || port <= 0) { send_result(p, RC_ERROR); return; }
#ifndef _WIN32
            if (tcp_dial(p, host, port) < 0) {
                send_result(p, RC_NO_CARRIER); return;
            }
            p->state = PERRYFI_STATE_ONLINE;
            p->esc_count = 0;
            send_result(p, RC_CONNECT);
#else
            send_result(p, RC_NO_CARRIER);
#endif
            return;
        }
        case '&':
            send_result(p, RC_OK); return;
        case '$':
            send_result(p, RC_OK); return;
        case 'X':
            p->extended_codes = (*s != '0');
            send_result(p, RC_OK); return;
        case 'S':
            send_result(p, RC_OK); return;
    }
    send_result(p, RC_ERROR);
}

static void run_at_line(Perryfi *p, char *line) {
    if (strcmp(line, "A/") == 0) {
        if (p->last_cmd[0]) { strcpy(line, p->last_cmd); }
        else { send_result(p, RC_ERROR); return; }
    }
    char *s = skip_ws(line);
    for (char *q = s; *q; q++) *q = upper(*q);
    if (s[0] != 'A' || s[1] != 'T') {
        send_result(p, RC_ERROR); return;
    }
    strncpy(p->last_cmd, line, sizeof(p->last_cmd) - 1);
    exec_at(p, s + 2);
}

static void maybe_echo(Perryfi *p, u8 b) {
    if (p->echo) rx_push(p, b);
}

/* Guest pushed one byte into the modem. */
bool perryfi_tx_push(Perryfi *p, u8 b) {
    if (!p->present) return false;

    if (p->state == PERRYFI_STATE_ONLINE) {
        Uint64 now = SDL_GetTicks();
        if (b == PERRYFI_ESC_CHAR &&
            (p->esc_count == 0 || (now - p->esc_last_ms) < 1000)) {
            p->esc_count++;
            p->esc_last_ms = now;
            if (p->esc_count >= PERRYFI_ESC_COUNT) {
                p->state = PERRYFI_STATE_CMD;
                p->esc_count = 0;
                send_result(p, RC_OK);
                return true;
            }
        } else {
            p->esc_count = 0;
        }
#ifndef _WIN32
        if (p->tcp_fd >= 0) {
            ssize_t n = send(p->tcp_fd, &b, 1, MSG_NOSIGNAL);
            if (n < 0) {
                tcp_close(p);
                p->state = PERRYFI_STATE_CMD;
                send_result(p, RC_NO_CARRIER);
            }
        }
#endif
        return true;
    }

    if (b == '\r') {
        p->cmd_buf[p->cmd_len] = 0;
        if (p->cmd_len > 0) {
            maybe_echo(p, b);
            run_at_line(p, p->cmd_buf);
        }
        p->cmd_len = 0;
        return true;
    }
    if (b == 0x08 || b == 0x7F) {
        if (p->cmd_len > 0) { p->cmd_len--; maybe_echo(p, b); }
        return true;
    }
    if (p->cmd_len < PERRYFI_CMD_MAX) {
        p->cmd_buf[p->cmd_len++] = (char)b;
        maybe_echo(p, b);
    }
    return true;
}

/* ---------------------------------------------------------------- */
/* Lifecycle + poll                                                 */

void perryfi_init(Perryfi *p, bool enable) {
    memset(p, 0, sizeof(*p));
    p->present        = enable;
    p->state          = PERRYFI_STATE_CMD;
    p->echo           = true;
    p->verbose        = true;
    p->extended_codes = false;
    p->tcp_fd         = -1;
}

void perryfi_shutdown(Perryfi *p) {
    tcp_close(p);
    p->present = false;
}

void perryfi_poll(Perryfi *p) {
    if (!p->present) return;
#ifndef _WIN32
    if (p->state == PERRYFI_STATE_ONLINE && p->tcp_fd >= 0) {
        for (int i = 0; i < 256; i++) {
            if (rb_space(p->rx_head, p->rx_tail) == 0) break;
            u8 b;
            ssize_t n = recv(p->tcp_fd, &b, 1, 0);
            if (n == 0) {
                tcp_close(p);
                p->state = PERRYFI_STATE_CMD;
                send_result(p, RC_NO_CARRIER);
                break;
            }
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                tcp_close(p);
                p->state = PERRYFI_STATE_CMD;
                send_result(p, RC_NO_CARRIER);
                break;
            }
            rx_push(p, b);
        }
    }
#endif
}

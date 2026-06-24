/* usifac.c — USIfAC II RS232 emulation. See usifac.h. */

#define _XOPEN_SOURCE 600   /* posix_openpt, grantpt, unlockpt, ptsname */
#define _DEFAULT_SOURCE     /* cfmakeraw */

#include "usifac.h"
#include "leds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#endif

#define MASK (USIFAC_RING - 1)

static inline size_t rb_count(size_t head, size_t tail) {
    return (head - tail) & MASK;
}
static inline size_t rb_space(size_t head, size_t tail) {
    return MASK - rb_count(head, tail);  /* one slot reserved to distinguish full/empty */
}
static inline bool rb_empty(size_t head, size_t tail) { return head == tail; }

static void rx_push(USIfAC *u, u8 b) {
    if (rb_space(u->rx_head, u->rx_tail) == 0) return;  /* drop on overflow */
    u->rx_buf[u->rx_head & MASK] = b;
    u->rx_head = (u->rx_head + 1) & MASK;
    leds_ping_split(LED_USIFAC, false);   /* RX (red) — host → CPC */
}
static bool rx_pop(USIfAC *u, u8 *out) {
    if (rb_empty(u->rx_head, u->rx_tail)) return false;
    *out = u->rx_buf[u->rx_tail & MASK];
    u->rx_tail = (u->rx_tail + 1) & MASK;
    return true;
}
static void tx_push(USIfAC *u, u8 b) {
    if (rb_space(u->tx_head, u->tx_tail) == 0) return;
    u->tx_buf[u->tx_head & MASK] = b;
    u->tx_head = (u->tx_head + 1) & MASK;
}
static bool tx_pop(USIfAC *u, u8 *out) {
    if (rb_empty(u->tx_head, u->tx_tail)) return false;
    *out = u->tx_buf[u->tx_tail & MASK];
    u->tx_tail = (u->tx_tail + 1) & MASK;
    return true;
}

#ifndef _WIN32

static int open_pty(USIfAC *u, const char *link_path) {
    int fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { perror("usifac: posix_openpt"); return -1; }
    if (grantpt(fd) < 0 || unlockpt(fd) < 0) {
        perror("usifac: grantpt/unlockpt"); close(fd); return -1;
    }
    const char *name = ptsname(fd);
    if (!name) { close(fd); return -1; }
    snprintf(u->pty_slave, sizeof(u->pty_slave), "%s", name);

    /* Open the slave ourselves and put it in raw mode. Two reasons:
     *  1. Without an open slave fd the device node may disappear from
     *     /dev/pts/ between user reconnects (devpts cleans up nodes that
     *     have no slave-side opener).
     *  2. PTY termios is attached to the slave's line discipline, not
     *     the master — setting it on the master is unreliable. Open the
     *     slave, force raw mode (no canonical buffering, no echo, no
     *     CR/LF translation, no signals), close immediately. The
     *     termios survives the close; subsequent slave opens by the
     *     user inherit it. */
    int sfd = open(name, O_RDWR | O_NOCTTY);
    if (sfd >= 0) {
        struct termios tio;
        if (tcgetattr(sfd, &tio) == 0) {
            cfmakeraw(&tio);
            tio.c_cflag |= CS8 | CREAD | CLOCAL;
            cfsetispeed(&tio, B115200);
            cfsetospeed(&tio, B115200);
            tcsetattr(sfd, TCSANOW, &tio);
        }
        close(sfd);
    }

    u->pty_master = fd;

    /* Optional stable host-side alias. Replaces any prior link (stale from a
     * crashed previous run is harmless). NULL/empty disables the symlink. */
    if (link_path && link_path[0]) {
        unlink(link_path);
        if (symlink(u->pty_slave, link_path) == 0) {
            snprintf(u->pty_link, sizeof(u->pty_link), "%s", link_path);
        } else {
            fprintf(stderr, "usifac: symlink(%s -> %s) failed: %s\n",
                    link_path, u->pty_slave, strerror(errno));
            u->pty_link[0] = '\0';
        }
    } else {
        u->pty_link[0] = '\0';
    }

    if (u->pty_link[0])
        fprintf(stderr, "usifac: PTY ready at %s (alias %s)\n",
                u->pty_slave, u->pty_link);
    else
        fprintf(stderr, "usifac: PTY ready at %s\n", u->pty_slave);
    return 0;
}

static int open_tcp(USIfAC *u, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("usifac: socket"); return -1; }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "usifac: bind localhost:%d failed: %s\n", port, strerror(errno));
        close(fd); return -1;
    }
    if (listen(fd, 1) < 0) { perror("usifac: listen"); close(fd); return -1; }

    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    u->tcp_listen = fd;
    u->tcp_port   = port;
    fprintf(stderr, "usifac: TCP listening on localhost:%d\n", port);
    return 0;
}

static void close_fd(int *pfd) {
    if (*pfd >= 0) { close(*pfd); *pfd = -1; }
}

#endif /* !_WIN32 */

void usifac_init(USIfAC *u, bool enable, const char *backend, int tcp_port,
                 const char *pty_link_path) {
    memset(u, 0, sizeof(*u));
    u->pty_master = -1;
    u->tcp_listen = -1;
    u->tcp_client = -1;
    u->present = enable;
    if (!u->present) return;

#ifdef _WIN32
    (void)backend; (void)tcp_port; (void)pty_link_path;
    fprintf(stderr, "usifac: backend not supported on Windows yet — disabling\n");
    u->present = false;
    return;
#else
    if (backend && !strcmp(backend, "tcp")) {
        u->backend = USIFAC_BACKEND_TCP;
        if (open_tcp(u, tcp_port) < 0) {
            u->present = false;
        }
    } else {
        u->backend = USIFAC_BACKEND_PTY;
        if (open_pty(u, pty_link_path) < 0) {
            u->present = false;
        }
    }
#endif
}

void usifac_shutdown(USIfAC *u) {
#ifndef _WIN32
    /* Best-effort cleanup of the stable alias — only if it still points at
     * the slave we created (don't clobber an unrelated file/link). */
    if (u->pty_link[0]) {
        char target[64];
        ssize_t n = readlink(u->pty_link, target, sizeof(target) - 1);
        if (n > 0) {
            target[n] = '\0';
            if (!strcmp(target, u->pty_slave))
                unlink(u->pty_link);
        }
        u->pty_link[0] = '\0';
    }
    close_fd(&u->pty_master);
    close_fd(&u->tcp_client);
    close_fd(&u->tcp_listen);
#endif
    u->present = false;
}

#ifndef _WIN32
static void poll_pty(USIfAC *u) {
    if (u->pty_master < 0) return;

    /* Drain backend → RX */
    while (rb_space(u->rx_head, u->rx_tail) > 0) {
        u8 c;
        ssize_t n = read(u->pty_master, &c, 1);
        if (n <= 0) break;
        rx_push(u, c);
    }

    /* Push TX → backend */
    u8 c;
    while (tx_pop(u, &c)) {
        ssize_t n = write(u->pty_master, &c, 1);
        if (n < 0) {
            /* Slave not attached or pipe full — drop. */
            break;
        }
        leds_ping_split(LED_USIFAC, true);   /* TX (green) — CPC → host */
    }
}

static void poll_tcp(USIfAC *u) {
    if (u->tcp_listen < 0) return;

    /* Accept incoming if no client */
    if (u->tcp_client < 0) {
        int fd = accept(u->tcp_listen, NULL, NULL);
        if (fd >= 0) {
            int fl = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, fl | O_NONBLOCK);
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            u->tcp_client = fd;
            fprintf(stderr, "usifac: TCP client connected\n");
        }
    }

    if (u->tcp_client < 0) return;

    /* Drain client → RX. recv()==0 means the client half-closed its send
     * side (FIN); we may still have TX bytes to deliver, so don't tear
     * down the connection here — just stop reading. The connection ends
     * for real when a send() below fails. */
    bool read_eof = false;
    while (!read_eof && rb_space(u->rx_head, u->rx_tail) > 0) {
        u8 c;
        ssize_t n = recv(u->tcp_client, &c, 1, 0);
        if (n == 0) { read_eof = true; break; }
        if (n < 0) break;
        rx_push(u, c);
    }

    /* Push TX → client */
    u8 c;
    while (tx_pop(u, &c)) {
        ssize_t n = send(u->tcp_client, &c, 1, MSG_NOSIGNAL);
        if (n < 0) {
            /* Peer is fully gone (EPIPE/ECONNRESET) — close and reopen
             * the accept loop. */
            close(u->tcp_client);
            u->tcp_client = -1;
            fprintf(stderr, "usifac: TCP client disconnected\n");
            return;
        }
        leds_ping_split(LED_USIFAC, true);   /* TX (green) — CPC → host */
    }
}
#endif /* !_WIN32 */

void usifac_poll(USIfAC *u) {
    if (!u->present) return;
#ifndef _WIN32
    if (u->backend == USIFAC_BACKEND_PTY) poll_pty(u);
    else                                  poll_tcp(u);
#else
    (void)u;
#endif
}

u8 usifac_read(USIfAC *u, u8 lo) {
    if (!u->present) return 0xFF;
    switch (lo & 0x0F) {
        case 0x0: {  /* &FBD0 — data */
            u8 b = 0;
            if (u->burst_mode) {
                /* Real chip holds /WAIT until 3100 bytes are buffered.
                 * We don't have /WAIT; busy-poll the backend a few
                 * times before giving up. */
                for (int spin = 0; spin < 64 && rb_empty(u->rx_head, u->rx_tail); spin++) {
                    usifac_poll(u);
                }
            }
            rx_pop(u, &b);
            return b;
        }
        case 0x1:  /* &FBD1 — status */
            return rb_empty(u->rx_head, u->rx_tail) ? 0x01 : 0xFF;
        case 0x8:  /* &FBD8 — presence (anything ≠ 0xFF is "present") */
            return 0x00;
        case 0xD:  /* &FBDD — current baud code */
            return u->baud_code;
        default:
            return 0xFF;
    }
}

void usifac_write(USIfAC *u, u8 lo, u8 val) {
    if (!u->present) return;
    switch (lo & 0x0F) {
        case 0x0:  /* &FBD0 — TX */
            tx_push(u, val);
            return;
        case 0x1:  /* &FBD1 — control */
            switch (val) {
                case 0:  /* reset interface (no CPC reset) */
                    u->rx_head = u->rx_tail = 0;
                    u->tx_head = u->tx_tail = 0;
                    u->burst_mode = false;
                    u->baud_code  = 0;
                    return;
                case 1:  /* clear RX */
                    u->rx_head = u->rx_tail = 0;
                    return;
                case 2:  /* burst mode on */
                    u->burst_mode = true;
                    return;
                case 3:  /* burst mode off */
                    u->burst_mode = false;
                    return;
                case 4:        /* disable Direct/FDC mode — host-ROM concern */
                case 65: case 66:  /* post-reset USB dir behaviour */
                    return;
                case 30:       /* |STAT request — stub a zeroed reply */
                    for (int i = 0; i < 8; i++) rx_push(u, 0);
                    return;
                default:
                    if (val >= 10 && val <= 23) {
                        u->baud_code = val;     /* readable at &FBDD */
                    }
                    return;
            }
        case 0x2:  /* &FBD2 — ROM-slot select (host-ROM concern; ignore) */
        default:
            return;
    }
}

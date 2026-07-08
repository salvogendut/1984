/* webgui.c — embedded Web GUI HTTP server. See webgui.h.
 *
 * Single-threaded, non-blocking, polled once per frame like the other
 * outside-world subsystems (net4cpc, usifac). A slow client applies its
 * own backpressure: a new video part is only queued once the previous
 * one has fully drained, so memory per client stays bounded at roughly
 * one encoded frame and laggy viewers simply receive fewer frames.
 */
#include "compat_win.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "webgui.h"
#include "webgui_page.h"
#include "display.h"
#include "gifcap.h"
#include "kbd.h"
#include "leds.h"
#include "notify.h"

#define WG_MAX_CLIENTS   8
#define WG_REQ_MAX       4096      /* request head + small POST body */
#define WG_PASTE_MAX     2048
#define WG_BOUNDARY      "1984frame"
#define WG_KEEPALIVE_MS  500       /* resend interval for static screens */

typedef enum {
    WC_FREE = 0,
    WC_READ_REQUEST,   /* accumulating request bytes */
    WC_STREAMING       /* /stream client: parts queued as they drain */
} WcState;

typedef struct {
    int      fd;                 /* -1 when WC_FREE */
    WcState  state;
    char     req[WG_REQ_MAX];
    int      req_len;
    uint8_t *out;                /* response bytes awaiting send */
    size_t   out_cap, out_len, out_off;
    bool     close_after_send;
    uint32_t sent_hash;          /* hash of last frame queued to this client */
    uint64_t sent_ms;
} WebClient;

static struct {
    CPC      *cpc;
    Paste    *paste;
    int       listen_fd;
    int       port;
    WebClient cl[WG_MAX_CLIENTS];
    GifCap   *enc;               /* shared single-frame encoder */
    const uint8_t *last_gif;     /* last encoded frame, owned by enc */
    size_t    last_gif_len;
    uint32_t  last_hash;         /* hash of last encoded frame */
    uint64_t  last_encode_ms;
    int       decim;             /* 50 -> 25 fps toggle */
    bool      joy_held[6];       /* web-held row-9 bits, released on stop */
} wg = { .listen_fd = -1 };

/* ---------- output buffer ---------- */

static bool wc_out(WebClient *c, const void *p, size_t n) {
    if (c->out_len + n > c->out_cap) {
        size_t cap = c->out_cap ? c->out_cap : 4096;
        while (cap < c->out_len + n) cap *= 2;
        uint8_t *nb = realloc(c->out, cap);
        if (!nb) return false;
        c->out = nb;
        c->out_cap = cap;
    }
    memcpy(c->out + c->out_len, p, n);
    c->out_len += n;
    return true;
}

static bool wc_printf(WebClient *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static bool wc_printf(WebClient *c, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0 || n >= (int)sizeof(buf)) return false;
    return wc_out(c, buf, (size_t)n);
}

static void wc_close(WebClient *c) {
    if (c->fd >= 0) sock_close(c->fd);
    free(c->out);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->state = WC_FREE;
}

/* Non-blocking drain of c->out; closes the slot on error or when a
 * close_after_send response finishes. */
static void wc_send(WebClient *c) {
    while (c->out_off < c->out_len) {
        ssize_t n = send(c->fd, (const char *)c->out + c->out_off,
                         c->out_len - c->out_off, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            c->out_off += (size_t)n;
            continue;
        }
        if (n < 0 && sock_would_block()) return;
        wc_close(c);
        return;
    }
    c->out_off = c->out_len = 0;
    if (c->close_after_send) wc_close(c);
}

/* ---------- tiny HTTP helpers ---------- */

static void respond_status(WebClient *c, const char *status, bool keep) {
    wc_printf(c, "HTTP/1.1 %s\r\nConnection: %s\r\nContent-Length: 0\r\n\r\n",
              status, keep ? "keep-alive" : "close");
    if (!keep) c->close_after_send = true;
}

static int lower(int ch) { return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch; }

static bool str_ieq(const char *a, const char *b) {
    while (*a && *b && lower(*a) == lower(*b)) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

static bool str_niq(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (lower(a[i]) != lower(b[i]) || a[i] == '\0') return a[i] == b[i];
    return true;
}

/* Case-insensitive header lookup inside the (NUL-terminated) request
 * head; value copied without surrounding whitespace. Returns false if
 * absent. */
static bool header_value(const char *head, const char *name,
                         char *out, size_t outsz) {
    size_t nlen = strlen(name);
    for (const char *p = strstr(head, "\r\n"); p; p = strstr(p, "\r\n")) {
        p += 2;
        if (str_niq(p, name, nlen) && p[nlen] == ':') {
            p += nlen + 1;
            while (*p == ' ' || *p == '\t') p++;
            size_t i = 0;
            while (p[i] && p[i] != '\r' && i < outsz - 1) { out[i] = p[i]; i++; }
            out[i] = '\0';
            return true;
        }
    }
    return false;
}

static int hexval(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* Extract and percent-decode one query parameter ("k=v&k2=v2"). */
static bool query_param(const char *query, const char *key,
                        char *out, size_t outsz) {
    size_t klen = strlen(key);
    const char *p = query;
    while (p && *p) {
        const char *next = strchr(p, '&');
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *v = p + klen + 1;
            const char *end = next ? next : v + strlen(v);
            size_t i = 0;
            while (v < end && i < outsz - 1) {
                if (*v == '+') { out[i++] = ' '; v++; }
                else if (*v == '%' && v + 2 < end &&
                         hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
                    out[i++] = (char)(hexval(v[1]) * 16 + hexval(v[2]));
                    v += 3;
                } else out[i++] = *v++;
            }
            out[i] = '\0';
            return true;
        }
        p = next ? next + 1 : NULL;
    }
    return false;
}

/* ---------- request routing ---------- */

static void route_page(WebClient *c) {
    wc_printf(c, "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/html; charset=utf-8\r\n"
                 "Cache-Control: no-store\r\n"
                 "Connection: keep-alive\r\n"
                 "Content-Length: %u\r\n\r\n", webgui_page_len);
    wc_out(c, webgui_page, webgui_page_len);
}

static void route_stream(WebClient *c) {
    wc_printf(c, "HTTP/1.1 200 OK\r\n"
                 "Content-Type: multipart/x-mixed-replace; boundary=" WG_BOUNDARY "\r\n"
                 "Cache-Control: no-store\r\n"
                 "Connection: close\r\n\r\n");
    c->state = WC_STREAMING;
    c->sent_hash = 0;
    c->sent_ms = 0;    /* keepalive rule pushes the first frame at once */
}

static void route_key(WebClient *c, const char *query) {
    char name[32], d[4];
    if (!query_param(query, "c", name, sizeof(name)) ||
        !query_param(query, "d", d, sizeof(d)) ||
        (d[0] != '0' && d[0] != '1') || d[1] != '\0') {
        respond_status(c, "400 Bad Request", true);
        return;
    }
    SDL_Scancode sc = SDL_GetScancodeFromName(name);
    if (sc == SDL_SCANCODE_UNKNOWN) {
        respond_status(c, "400 Bad Request", true);
        return;
    }
    cpc_key_event(wg.cpc, sc, d[0] == '1');
    respond_status(c, "204 No Content", true);
}

static void route_joy(WebClient *c, const char *query) {
    char b[4], d[4];
    if (!query_param(query, "b", b, sizeof(b)) ||
        !query_param(query, "d", d, sizeof(d)) ||
        b[0] < '0' || b[0] > '5' || b[1] != '\0' ||
        (d[0] != '0' && d[0] != '1') || d[1] != '\0') {
        respond_status(c, "400 Bad Request", true);
        return;
    }
    int bit = b[0] - '0';
    if (d[0] == '1') kbd_key_down(&wg.cpc->kbd, 9, bit);
    else             kbd_key_up(&wg.cpc->kbd, 9, bit);
    wg.joy_held[bit] = (d[0] == '1');
    respond_status(c, "204 No Content", true);
}

static void route_paste(WebClient *c, const char *body, int body_len) {
    char text[WG_PASTE_MAX + 1];
    int n = 0;
    for (int i = 0; i < body_len && n < WG_PASTE_MAX; i++) {
        unsigned char ch = (unsigned char)body[i];
        if (ch >= 0x20 || ch == '\n' || ch == '\t')
            text[n++] = (char)ch;
    }
    text[n] = '\0';
    if (n > 0) paste_text(wg.paste, text);
    respond_status(c, "204 No Content", true);
}

static void route(WebClient *c, const char *method, char *path,
                  const char *body, int body_len) {
    char *query = strchr(path, '?');
    if (query) *query++ = '\0';
    else query = path + strlen(path);   /* empty string */

    bool get  = strcmp(method, "GET") == 0;
    bool post = strcmp(method, "POST") == 0;
    if (!get && !post) {
        respond_status(c, "405 Method Not Allowed", false);
        return;
    }

    if (strcmp(path, "/") == 0 && get)           route_page(c);
    else if (strcmp(path, "/stream") == 0 && get) route_stream(c);
    else if (strcmp(path, "/key") == 0)          route_key(c, query);
    else if (strcmp(path, "/joy") == 0)          route_joy(c, query);
    else if (strcmp(path, "/paste") == 0)        route_paste(c, body, body_len);
    else if (strcmp(path, "/reset") == 0) {
        cpc_reset(wg.cpc);
        respond_status(c, "204 No Content", true);
    } else respond_status(c, "404 Not Found", true);
}

/* Try to parse and dispatch one complete request from c->req. Returns
 * false if more bytes are needed. */
static bool try_dispatch(WebClient *c) {
    c->req[c->req_len] = '\0';
    char *head_end = strstr(c->req, "\r\n\r\n");
    if (!head_end) {
        if (c->req_len >= WG_REQ_MAX - 1) {
            respond_status(c, "431 Request Header Fields Too Large", false);
            return true;
        }
        return false;   /* head still arriving */
    }
    int head_len = (int)(head_end - c->req) + 4;

    /* Terminate the head so header lookups can't scan into the body. */
    c->req[head_len - 2] = '\0';
    int content_len = 0;
    char clv[16], conn[16];
    if (header_value(c->req, "Content-Length", clv, sizeof(clv)))
        content_len = atoi(clv);
    bool asked_close = header_value(c->req, "Connection", conn, sizeof(conn)) &&
                       str_ieq(conn, "close");
    c->req[head_len - 2] = '\r';

    if (content_len < 0 || head_len + content_len > WG_REQ_MAX - 1) {
        respond_status(c, "413 Content Too Large", false);
        return true;
    }
    if (c->req_len < head_len + content_len)
        return false;   /* body still arriving */

    char method[8] = "", target[512] = "";
    if (sscanf(c->req, "%7s %511s", method, target) != 2) {
        respond_status(c, "400 Bad Request", false);
        return true;
    }

    route(c, method, target, c->req + head_len, content_len);
    if (asked_close && c->state != WC_STREAMING)
        c->close_after_send = true;

    /* Keep-alive: shift any pipelined leftovers to the front. */
    int consumed = head_len + content_len;
    int leftover = c->req_len - consumed;
    if (leftover > 0 && c->state == WC_READ_REQUEST)
        memmove(c->req, c->req + consumed, (size_t)leftover);
    c->req_len = (c->state == WC_READ_REQUEST) ? leftover : 0;
    return true;
}

/* ---------- public API ---------- */

void webgui_init(CPC *cpc, Paste *paste) {
    wg.cpc = cpc;
    wg.paste = paste;
    wg.listen_fd = -1;
    for (int i = 0; i < WG_MAX_CLIENTS; i++) wg.cl[i].fd = -1;
}

bool webgui_start(int port) {
    if (wg.listen_fd >= 0) return true;
    if (port < 1 || port > 65535) return false;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        notify_post("Web GUI: socket failed");
        return false;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
        listen(fd, 4) < 0) {
        notify_post("Web GUI: cannot listen on port %d", port);
        sock_close(fd);
        return false;
    }
    sock_set_nonblocking(fd);

    wg.enc = gifcap_open_mem(CPC_SCREEN_W, CPC_SCREEN_H,
                             CPC_SCREEN_W, CPC_SCREEN_H, 4);
    if (!wg.enc) {
        sock_close(fd);
        return false;
    }
    wg.listen_fd = fd;
    wg.port = port;
    wg.last_gif = NULL;
    wg.last_gif_len = 0;
    wg.last_hash = 0;
    wg.last_encode_ms = 0;
    notify_post("Web GUI: listening on 0.0.0.0:%d", port);
    return true;
}

void webgui_stop(void) {
    if (wg.listen_fd < 0) return;
    for (int i = 0; i < WG_MAX_CLIENTS; i++)
        if (wg.cl[i].fd >= 0) wc_close(&wg.cl[i]);
    sock_close(wg.listen_fd);
    wg.listen_fd = -1;
    wg.port = 0;
    gifcap_close(wg.enc);
    wg.enc = NULL;
    wg.last_gif = NULL;
    wg.last_gif_len = 0;
    if (wg.cpc)
        for (int b = 0; b < 6; b++)
            if (wg.joy_held[b]) kbd_key_up(&wg.cpc->kbd, 9, b);
    memset(wg.joy_held, 0, sizeof(wg.joy_held));
    notify_post("Web GUI: stopped");
}

bool webgui_active(void) { return wg.listen_fd >= 0; }
int  webgui_port(void)   { return wg.listen_fd >= 0 ? wg.port : 0; }

void webgui_poll(void) {
    if (wg.listen_fd < 0) return;

    /* Accept new connections. */
    for (;;) {
        int fd = accept(wg.listen_fd, NULL, NULL);
        if (fd < 0) break;
        WebClient *c = NULL;
        for (int i = 0; i < WG_MAX_CLIENTS; i++)
            if (wg.cl[i].state == WC_FREE) { c = &wg.cl[i]; break; }
        if (!c) {
            static const char busy[] =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Connection: close\r\nContent-Length: 0\r\n\r\n";
            send(fd, busy, sizeof(busy) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
            sock_close(fd);
            continue;
        }
        sock_set_nonblocking(fd);
        c->fd = fd;
        c->state = WC_READ_REQUEST;
        leds_ping(LED_NET);
    }

    /* Read + dispatch. */
    for (int i = 0; i < WG_MAX_CLIENTS; i++) {
        WebClient *c = &wg.cl[i];
        if (c->state == WC_FREE) continue;
        for (;;) {
            char tmp[1024];
            char *dst = tmp;
            int room = (int)sizeof(tmp);
            if (c->state == WC_READ_REQUEST) {
                dst = c->req + c->req_len;
                room = WG_REQ_MAX - 1 - c->req_len;
                if (room <= 0) break;   /* full head handled in try_dispatch */
            }
            ssize_t n = recv(c->fd, dst, (size_t)room, MSG_DONTWAIT);
            if (n == 0 || (n < 0 && !sock_would_block())) {
                wc_close(c);
                break;
            }
            if (n < 0) break;
            if (c->state == WC_READ_REQUEST) {
                c->req_len += (int)n;
                /* Dispatch every complete pipelined request in the buffer. */
                while (c->state == WC_READ_REQUEST && c->req_len > 0 &&
                       !c->close_after_send && try_dispatch(c))
                    ;
            }
            /* WC_STREAMING: incoming bytes are discarded. */
        }
    }

    /* Drain output buffers. */
    for (int i = 0; i < WG_MAX_CLIENTS; i++)
        if (wg.cl[i].state != WC_FREE && wg.cl[i].out_len > wg.cl[i].out_off)
            wc_send(&wg.cl[i]);
}

void webgui_frame(void) {
    if (wg.listen_fd < 0 || !wg.enc) return;
    if ((wg.decim ^= 1) != 0) return;   /* 25 fps cap */

    uint64_t now = SDL_GetTicks();

    /* Any streaming client ready for a new part? */
    bool want = false;
    for (int i = 0; i < WG_MAX_CLIENTS; i++) {
        WebClient *c = &wg.cl[i];
        if (c->state == WC_STREAMING && c->out_len == c->out_off)
            want = true;
    }
    if (!want) return;

    uint32_t hash = display_hash(&wg.cpc->display);
    if (!wg.last_gif || hash != wg.last_hash ||
        now - wg.last_encode_ms >= WG_KEEPALIVE_MS) {
        wg.last_gif = gifcap_encode_single(wg.enc, wg.cpc->display.pixels,
                                           &wg.last_gif_len);
        if (!wg.last_gif) return;
        wg.last_hash = hash;
        wg.last_encode_ms = now;
    }

    for (int i = 0; i < WG_MAX_CLIENTS; i++) {
        WebClient *c = &wg.cl[i];
        if (c->state != WC_STREAMING || c->out_len != c->out_off) continue;
        if (c->sent_hash == wg.last_hash && now - c->sent_ms < WG_KEEPALIVE_MS)
            continue;
        c->out_off = c->out_len = 0;
        wc_printf(c, "--" WG_BOUNDARY "\r\n"
                     "Content-Type: image/gif\r\n"
                     "Content-Length: %zu\r\n\r\n", wg.last_gif_len);
        wc_out(c, wg.last_gif, wg.last_gif_len);
        wc_out(c, "\r\n", 2);
        c->sent_hash = wg.last_hash;
        c->sent_ms = now;
        wc_send(c);
        leds_ping(LED_NET);
    }
}

/* websvc.c — Web Service: the multi-session "emulator as a service" HTTP
 * server started by --web. See websvc.h.
 *
 * Structurally a copy-adapted sibling of webgui.c (the single-shared-CPC
 * server behind the F9 overlay toggle), not a refactor of it: the two
 * servers have different enough semantics (session/cookie routing,
 * capacity, idle eviction vs none of that) that sharing the ~150 lines of
 * HTTP parsing/response plumbing would cost more than it saves. Owns its
 * own SDL_Init, listening socket, and 50 Hz pacing loop — main.c dispatches
 * straight here for --web and never reaches the classic single-CPC path.
 */
#include "compat_win.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <ifaddrs.h>
#include <sys/stat.h>
#include <sys/random.h>
#include <dirent.h>
#endif

#include <SDL3/SDL.h>

#include "websvc.h"
#include "cpc.h"
#include "config.h"
#include "paste.h"
#include "display.h"
#include "gifcap.h"
#include "kbd.h"

#define WS_MAX_SESSIONS      4
#define WS_IDLE_TIMEOUT_MS   (10 * 60 * 1000)
#define WS_SWEEP_INTERVAL_MS 1000

#define WS_MAX_CLIENTS       16        /* shared pool across all sessions */
#define WS_REQ_MAX           4096
#define WS_PASTE_MAX         2048
#define WS_UPLOAD_MAX        (8 * 1024 * 1024)   /* .dsk or .conf body cap */
#define WS_BOUNDARY          "1984frame"
#define WS_KEEPALIVE_MS      500
#define WS_COOKIE_NAME       "1984sid"
#define WS_TOKEN_LEN         32        /* 128-bit token, hex-encoded */

typedef struct Session {
    bool      in_use;
    CPC       cpc;
    Config    cfg;
    Paste     paste;
    GifCap   *enc;
    const uint8_t *last_gif;
    size_t    last_gif_len;
    uint32_t  last_hash;
    uint64_t  last_encode_ms;
    uint8_t   audio_bytes[CPC_AUDIO_FRAME_CAPACITY * 2 * sizeof(s16)];
    size_t    audio_len;
    uint64_t  audio_seq;
    int       decim;
    bool      joy_held[6];
    bool      mbtn_held[3];
    char      cookie[WS_TOKEN_LEN + 1];
    char      upload_dir[CONFIG_PATH_MAX];
    uint64_t  created_ms;
    uint64_t  last_attached_ms;   /* refreshed whenever a streaming client attaches */
    int       attached;           /* currently attached video/audio stream clients */
} Session;

typedef enum {
    WC_FREE = 0,
    WC_READ_REQUEST,
    WC_READ_UPLOAD,
    WC_STREAMING,
    WC_AUDIO_STREAM
} WcState;

typedef enum { WS_UPLOAD_DISK, WS_UPLOAD_CONFIG } UploadKind;

typedef struct {
    int      fd;
    char     ip[16];
    WcState  state;
    char     req[WS_REQ_MAX];
    int      req_len;
    Session *sess;               /* resolved once the Cookie header is parsed */
    uint8_t *upload;
    size_t   upload_len, upload_cap;
    UploadKind upload_kind;
    int      upload_drive;
    char     upload_name[64];
    uint8_t *out;
    size_t   out_cap, out_len, out_off;
    bool     close_after_send;
    uint32_t sent_hash;
    uint64_t sent_ms;
    uint64_t sent_audio_seq;
} WebClient;

/* The page is identical to the Web GUI's; webgui_page.h *defines* (not just
 * declares) webgui_page[]/webgui_page_len, and it's already compiled into
 * webgui.c — reference those symbols instead of re-including the header,
 * which would duplicate-define them at link time. */
extern unsigned char webgui_page[];
extern unsigned int  webgui_page_len;

static Session   g_sessions[WS_MAX_SESSIONS];
static WebClient g_clients[WS_MAX_CLIENTS];
static int       g_listen_fd = -1;
static uint64_t  g_last_sweep_ms;

static void session_audio_sink(void *userdata, const s16 *samples,
                               int frames, int sample_rate) {
    Session *s = userdata;
    if (!s || sample_rate != CPC_AUDIO_SAMPLE_RATE ||
        !samples || frames <= 0)
        return;
    if (frames > CPC_AUDIO_FRAME_CAPACITY)
        frames = CPC_AUDIO_FRAME_CAPACITY;
    int nsamples = frames * 2;
    for (int i = 0; i < nsamples; i++) {
        uint16_t v = (uint16_t)samples[i];
        s->audio_bytes[i * 2] = (uint8_t)v;
        s->audio_bytes[i * 2 + 1] = (uint8_t)(v >> 8);
    }
    s->audio_len = (size_t)nsamples * 2u;
    s->audio_seq++;
}

/* ---------- logging (always on — this IS the headless daemon) ---------- */

static void wlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void wlog(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "1984 websvc: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static void wlog_urls(int port) {
#ifndef _WIN32
    struct ifaddrs *list;
    if (getifaddrs(&list) != 0) return;
    for (struct ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        const struct sockaddr_in *sa = (const struct sockaddr_in *)ifa->ifa_addr;
        char ip[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip))) continue;
        wlog("  http://%s:%d/  (%s)", ip, port, ifa->ifa_name);
    }
    freeifaddrs(list);
#else
    (void)port;
#endif
}

/* ---------- graceful shutdown ---------- */

static volatile sig_atomic_t g_running = 1;
static void on_term_signal(int sig) { (void)sig; g_running = 0; }

/* ---------- output buffer (identical shape to webgui.c) ---------- */

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

static void le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wav_stream_header(uint8_t hdr[44]) {
    const uint32_t data_bytes = 0x7fffffffU - 36U;
    memcpy(hdr + 0, "RIFF", 4);
    le32(hdr + 4, 36U + data_bytes);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    le32(hdr + 16, 16);
    le16(hdr + 20, 1);                         /* PCM */
    le16(hdr + 22, 2);                         /* stereo */
    le32(hdr + 24, CPC_AUDIO_SAMPLE_RATE);
    le32(hdr + 28, CPC_AUDIO_SAMPLE_RATE * 2U * 16U / 8U);
    le16(hdr + 32, 2U * 16U / 8U);             /* block align */
    le16(hdr + 34, 16);                        /* bits per sample */
    memcpy(hdr + 36, "data", 4);
    le32(hdr + 40, data_bytes);
}

static void wc_close(WebClient *c) {
    if ((c->state == WC_STREAMING || c->state == WC_AUDIO_STREAM) && c->sess) {
        c->sess->attached--;
        c->sess->last_attached_ms = SDL_GetTicks();
        wlog("session %s: %s stream %s disconnected", c->sess->cookie,
             c->state == WC_AUDIO_STREAM ? "audio" : "video", c->ip);
    }
    if (c->fd >= 0) sock_close(c->fd);
    free(c->out);
    free(c->upload);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->state = WC_FREE;
}

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

/* ---------- tiny HTTP helpers (copy-adapted from webgui.c) ---------- */

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

/* Cookie header looks like "a=1; 1984sid=<token>; b=2" — find our name
 * among the ';'-separated pairs. */
static bool cookie_token(const char *cookie_header, char *out, size_t outsz) {
    size_t nlen = strlen(WS_COOKIE_NAME);
    const char *p = cookie_header;
    while (p && *p) {
        while (*p == ' ' || *p == ';') p++;
        if (strncmp(p, WS_COOKIE_NAME, nlen) == 0 && p[nlen] == '=') {
            const char *v = p + nlen + 1;
            size_t i = 0;
            while (v[i] && v[i] != ';' && v[i] != ' ' && i < outsz - 1) {
                out[i] = v[i];
                i++;
            }
            out[i] = '\0';
            return i > 0;
        }
        p = strchr(p, ';');
        if (p) p++;
    }
    return false;
}

/* Session cookies gate full control of a session (keystrokes, mouse, disk
 * mount, config rebuild) purely by possession, so the token must not be
 * guessable -- draw raw bytes from the OS CSPRNG rather than libc rand(),
 * which is unsuitable for anything security-sensitive. */
static void gen_token(char *out) {
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[WS_TOKEN_LEN / 2];
    bool have_entropy = false;
#ifndef _WIN32
    have_entropy = getentropy(raw, sizeof(raw)) == 0;
#endif
    if (!have_entropy) {
        /* Fallback for platforms/sandboxes without getentropy(): still
         * seed once from a high-resolution counter, but this path should
         * not be reachable on any of our supported targets. */
        static bool seeded = false;
        if (!seeded) { srand((unsigned)SDL_GetPerformanceCounter()); seeded = true; }
        for (size_t i = 0; i < sizeof(raw); i++) raw[i] = (unsigned char)rand();
    }
    for (int i = 0; i < WS_TOKEN_LEN / 2; i++) {
        out[i * 2]     = hex[raw[i] >> 4];
        out[i * 2 + 1] = hex[raw[i] & 0xF];
    }
    out[WS_TOKEN_LEN] = '\0';
}

/* ---------- session lifecycle ---------- */

static Session *session_lookup(const char *token) {
    if (!token || !token[0]) return NULL;
    for (int i = 0; i < WS_MAX_SESSIONS; i++)
        if (g_sessions[i].in_use && strcmp(g_sessions[i].cookie, token) == 0)
            return &g_sessions[i];
    return NULL;
}

static void session_release_input(Session *s) {
    for (int b = 0; b < 6; b++)
        if (s->joy_held[b]) kbd_key_up(&s->cpc.kbd, 9, b);
    for (int b = 0; b < 3; b++) {
        if (!s->mbtn_held[b]) continue;
        if (s->cpc.symbiface_mouse) mouse_button(&s->cpc.mouse, b, false);
        if (s->cpc.albireo_mouse)   ch376_mouse_button(&s->cpc.ch376, b, false);
        if (s->cpc.amx_mouse)       amx_button(&s->cpc.amx, &s->cpc.kbd, b, false);
    }
}

#ifndef _WIN32
static void rm_rf_shallow(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char path[CONFIG_PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        remove(path);
    }
    closedir(d);
    rmdir(dir);
}
#else
static void rm_rf_shallow(const char *dir) { (void)dir; }
#endif

static void session_destroy(Session *s) {
    /* Close every client attached to this session first. */
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
        if (g_clients[i].state != WC_FREE && g_clients[i].sess == s)
            wc_close(&g_clients[i]);
    session_release_input(s);
    usifac_shutdown(&s->cpc.usifac);
    perryfi_shutdown(&s->cpc.perryfi);
    gifcap_close(s->enc);
    paste_free(&s->paste);
    cpc_destroy(&s->cpc);
    rm_rf_shallow(s->upload_dir);
    wlog("session %s destroyed", s->cookie);
    memset(s, 0, sizeof(*s));
    s->in_use = false;
}

/* Reap the oldest idle-evictable session (0 attached viewers, past the
 * grace/idle window). Returns true if a slot was freed. */
static bool session_reap_one_idle(uint64_t now) {
    Session *victim = NULL;
    for (int i = 0; i < WS_MAX_SESSIONS; i++) {
        Session *s = &g_sessions[i];
        if (!s->in_use || s->attached > 0) continue;
        if (now - s->last_attached_ms < WS_IDLE_TIMEOUT_MS) continue;
        if (!victim || s->last_attached_ms < victim->last_attached_ms)
            victim = s;
    }
    if (!victim) return false;
    session_destroy(victim);
    return true;
}

static void session_idle_sweep(uint64_t now) {
    for (int i = 0; i < WS_MAX_SESSIONS; i++) {
        Session *s = &g_sessions[i];
        if (!s->in_use || s->attached > 0) continue;
        uint64_t since = now - s->last_attached_ms;
        if (since >= WS_IDLE_TIMEOUT_MS)
            session_destroy(s);
    }
}

/* Build a session's CPC/Paste/encoder state from cfg (already loaded by
 * the caller — either config_defaults() for a fresh session or an
 * uploaded 1984.conf for a rebuild). Returns false on cpc_init failure. */
static bool session_build(Session *s, Config *cfg) {
    s->cfg = *cfg;
    if (cpc_build_from_config(&s->cpc, &s->cfg) < 0) return false;
    paste_init(&s->paste);
    s->enc = gifcap_open_mem(CPC_SCREEN_W, CPC_SCREEN_H,
                             CPC_SCREEN_W, CPC_SCREEN_H, 4);
    if (!s->enc) {
        cpc_destroy(&s->cpc);
        return false;
    }
    s->last_gif = NULL;
    s->last_gif_len = 0;
    s->last_hash = 0;
    s->last_encode_ms = 0;
    s->audio_len = 0;
    s->audio_seq = 0;
    cpc_set_audio_sink(&s->cpc, session_audio_sink, s);
    s->decim = 0;
    memset(s->joy_held, 0, sizeof(s->joy_held));
    memset(s->mbtn_held, 0, sizeof(s->mbtn_held));
    return true;
}

/* GET "/" with no (or a stale) cookie: create a session, subject to
 * capacity. Returns NULL (caller sends 503) if the daemon is full and no
 * idle session could be reaped. */
static Session *session_create(uint64_t now) {
    Session *slot = NULL;
    for (int i = 0; i < WS_MAX_SESSIONS; i++)
        if (!g_sessions[i].in_use) { slot = &g_sessions[i]; break; }
    if (!slot) {
        if (!session_reap_one_idle(now)) return NULL;
        for (int i = 0; i < WS_MAX_SESSIONS; i++)
            if (!g_sessions[i].in_use) { slot = &g_sessions[i]; break; }
        if (!slot) return NULL;
    }

    Config cfg;
    config_defaults(&cfg);   /* daemon sessions never read the host's real config */
    if (!session_build(slot, &cfg)) return NULL;

    gen_token(slot->cookie);
    char base[CONFIG_PATH_MAX];
    if (config_websvc_dir(base, sizeof(base)) == 0) {
        snprintf(slot->upload_dir, sizeof(slot->upload_dir), "%s/%s",
                 base, slot->cookie);
        mkdir(slot->upload_dir, 0700);
    }
    slot->created_ms = now;
    slot->last_attached_ms = now;   /* seeded to now: a session isn't reaped
                                      * before a viewer even connects — same
                                      * idle window just doubles as the grace
                                      * period */
    slot->attached = 0;
    slot->in_use = true;

    int live = 0;
    for (int i = 0; i < WS_MAX_SESSIONS; i++)
        if (g_sessions[i].in_use) live++;
    wlog("session %s created (%d/%d)", slot->cookie, live, WS_MAX_SESSIONS);
    return slot;
}

/* ---------- request routing ---------- */

static void route_page(WebClient *c, const char *token) {
    wc_printf(c, "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/html; charset=utf-8\r\n"
                 "Cache-Control: no-store\r\n"
                 "Set-Cookie: " WS_COOKIE_NAME "=%s; Path=/; HttpOnly; SameSite=Lax\r\n"
                 "Connection: keep-alive\r\n"
                 "Content-Length: %u\r\n\r\n", token, webgui_page_len);
    wc_out(c, webgui_page, webgui_page_len);
}

static void route_busy(WebClient *c) {
    static const char body[] =
        "<!doctype html><html><body style=\"font:16px sans-serif;"
        "background:#101014;color:#d8d8e0;padding:2em\">"
        "<h1>All sessions busy</h1><p>This 1984 Web Service is already "
        "running the maximum number of concurrent sessions. Try again "
        "in a few minutes — idle sessions are freed automatically.</p>"
        "</body></html>";
    wc_printf(c, "HTTP/1.1 503 Service Unavailable\r\n"
                 "Content-Type: text/html; charset=utf-8\r\n"
                 "Connection: close\r\nContent-Length: %zu\r\n\r\n",
              sizeof(body) - 1);
    wc_out(c, body, sizeof(body) - 1);
    c->close_after_send = true;
}

static void route_stream(WebClient *c) {
    Session *s = c->sess;
    wc_printf(c, "HTTP/1.1 200 OK\r\n"
                 "Content-Type: multipart/x-mixed-replace; boundary=" WS_BOUNDARY "\r\n"
                 "Cache-Control: no-store\r\n"
                 "Connection: close\r\n\r\n");
    c->state = WC_STREAMING;
    c->sent_hash = 0;
    c->sent_ms = 0;
    s->attached++;
    s->last_attached_ms = SDL_GetTicks();
    wlog("session %s: viewer %s started streaming", s->cookie, c->ip);
}

static void route_audio(WebClient *c) {
    Session *s = c->sess;
    uint8_t hdr[44];
    wav_stream_header(hdr);
    wc_printf(c, "HTTP/1.1 200 OK\r\n"
                 "Content-Type: audio/wav\r\n"
                 "Cache-Control: no-store\r\n"
                 "Connection: close\r\n\r\n");
    wc_out(c, hdr, sizeof(hdr));
    c->state = WC_AUDIO_STREAM;
    c->sent_audio_seq = 0;
    s->attached++;
    s->last_attached_ms = SDL_GetTicks();
    wlog("session %s: audio stream %s started", s->cookie, c->ip);
}

static void route_key(Session *s, WebClient *c, const char *query) {
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
    cpc_key_event(&s->cpc, sc, d[0] == '1');
    respond_status(c, "204 No Content", true);
}

static void route_joy(Session *s, WebClient *c, const char *query) {
    char b[4], d[4];
    if (!query_param(query, "b", b, sizeof(b)) ||
        !query_param(query, "d", d, sizeof(d)) ||
        b[0] < '0' || b[0] > '5' || b[1] != '\0' ||
        (d[0] != '0' && d[0] != '1') || d[1] != '\0') {
        respond_status(c, "400 Bad Request", true);
        return;
    }
    int bit = b[0] - '0';
    if (d[0] == '1') kbd_key_down(&s->cpc.kbd, 9, bit);
    else             kbd_key_up(&s->cpc.kbd, 9, bit);
    s->joy_held[bit] = (d[0] == '1');
    respond_status(c, "204 No Content", true);
}

static bool session_has_pointer(Session *s) {
    return s->cpc.symbiface_mouse || s->cpc.albireo_mouse || s->cpc.amx_mouse;
}

static void route_mouse(Session *s, WebClient *c, const char *query) {
    char v[16], b[4], d[4];
    bool any = false;

    int dx = 0, dy = 0;
    if (query_param(query, "dx", v, sizeof(v))) { dx = atoi(v); any = true; }
    if (query_param(query, "dy", v, sizeof(v))) { dy = atoi(v); any = true; }
    if (dx < -1024 || dx > 1024 || dy < -1024 || dy > 1024) {
        respond_status(c, "400 Bad Request", true);
        return;
    }
    if (dx || dy) {
        if (s->cpc.symbiface_mouse) mouse_move(&s->cpc.mouse, dx, dy);
        if (s->cpc.albireo_mouse)   ch376_mouse_move(&s->cpc.ch376, dx, dy);
        if (s->cpc.amx_mouse)       amx_move(&s->cpc.amx, dx, dy);
    }

    if (query_param(query, "dz", v, sizeof(v))) {
        int dz = atoi(v);
        if (dz < -16 || dz > 16) {
            respond_status(c, "400 Bad Request", true);
            return;
        }
        if (dz && s->cpc.symbiface_mouse) mouse_scroll(&s->cpc.mouse, dz);
        any = true;
    }

    if (query_param(query, "b", b, sizeof(b)) &&
        query_param(query, "d", d, sizeof(d))) {
        if (b[0] < '0' || b[0] > '2' || b[1] != '\0' ||
            (d[0] != '0' && d[0] != '1') || d[1] != '\0') {
            respond_status(c, "400 Bad Request", true);
            return;
        }
        int btn = b[0] - '0';
        bool pressed = (d[0] == '1');
        if (s->cpc.symbiface_mouse) mouse_button(&s->cpc.mouse, btn, pressed);
        if (s->cpc.albireo_mouse)   ch376_mouse_button(&s->cpc.ch376, btn, pressed);
        if (s->cpc.amx_mouse)       amx_button(&s->cpc.amx, &s->cpc.kbd, btn, pressed);
        s->mbtn_held[btn] = pressed;
        any = true;
    }

    respond_status(c, any ? "204 No Content" : "400 Bad Request", true);
}

static void route_status(Session *s, WebClient *c) {
    char body[64];
    /* "sessions":true tells the page it's talking to the Web Service (this
     * module), not the single-shared-CPC Web GUI — enables the per-session
     * config upload control, which has no equivalent there. */
    int n = snprintf(body, sizeof(body), "{\"mouse\":%s,\"sessions\":true}",
                     session_has_pointer(s) ? "true" : "false");
    wc_printf(c, "HTTP/1.1 200 OK\r\n"
                 "Content-Type: application/json\r\n"
                 "Cache-Control: no-store\r\n"
                 "Connection: keep-alive\r\n"
                 "Content-Length: %d\r\n\r\n", n);
    wc_out(c, body, (size_t)n);
}

static bool mount_disk_bytes(Session *s, int drive, const void *data, size_t len,
                             const char *name) {
    if (!s->upload_dir[0]) return false;
    char path[CONFIG_PATH_MAX];
    snprintf(path, sizeof(path), "%s/drive_%c.dsk", s->upload_dir,
             drive == 0 ? 'a' : 'b');
    bool ok = false;
    FILE *f = fopen(path, "wb");
    if (f) {
        size_t w = fwrite(data, 1, len, f);
        fclose(f);
        ok = (w == len);
    }
    if (ok) {
        Disk *d = &s->cpc.drive[drive];
        disk_eject(d);
        ok = disk_load(d, path) == 0;
        if (ok) {
            char *dest = (drive == 0) ? s->cfg.disk_a : s->cfg.disk_b;
            snprintf(dest, CONFIG_PATH_MAX, "%s", path);
        }
    }
    wlog(ok ? "session %s: drive %c mounted \"%s\" (%zu bytes, uploaded)"
            : "session %s: drive %c failed to mount uploaded \"%s\"",
         s->cookie, drive == 0 ? 'A' : 'B', name, len);
    return ok;
}

/* /session/config lets an anonymous remote client upload a 1984.conf to
 * retune display/audio/hardware settings for their own session -- but every
 * path-typed field (disk/ROM/tape/printer/PTY-link/SD-card image) resolves
 * to a host filesystem location when cpc_build_from_config() runs. Left
 * unchecked, a client could point e.g. PdfPrinterDir or DriveA at an
 * arbitrary host path -- a real file write/read primitive. Disk mounting
 * already has its own sandboxed endpoint (/disk, writes under
 * s->upload_dir), so no legitimate use of this endpoint needs to set any of
 * these fields: keep whatever the session already had for all of them and
 * only let the upload affect non-path settings. */
static void preserve_host_paths(Config *cfg, const Config *cur) {
#define KEEP(field) memcpy(cfg->field, cur->field, sizeof(cfg->field))
    KEEP(rom_os); KEEP(rom_basic); KEEP(rom_amsdos);
    KEEP(rom_ext); KEEP(rom_ext_boards);
    KEEP(board_m4_slot); KEEP(board_albireo_slot); KEEP(board_cyboard_slot);
    KEEP(board_m4_image); KEEP(board_albireo_image); KEEP(board_cyboard_image);
    KEEP(disk_a); KEEP(disk_b); KEEP(tape);
    KEEP(m4_path); KEEP(m4_image);
    KEEP(usifac_pty_link);
    KEEP(ide_image);
    KEEP(albireo_image);
    KEEP(pdf_printer_dir);
    KEEP(last_dir);
#undef KEEP
}

/* Tear this session's CPC down and rebuild it from an uploaded 1984.conf,
 * in place — same cookie, same slot, same clients. */
static bool rebuild_from_config_file(Session *s, const char *path) {
    Config cfg;
    if (config_load_from(&cfg, path) < 0) return false;
    preserve_host_paths(&cfg, &s->cfg);
    session_release_input(s);
    usifac_shutdown(&s->cpc.usifac);
    perryfi_shutdown(&s->cpc.perryfi);
    gifcap_close(s->enc);
    paste_free(&s->paste);
    cpc_destroy(&s->cpc);
    if (!session_build(s, &cfg)) {
        wlog("session %s: rebuild from uploaded config FAILED", s->cookie);
        return false;
    }
    wlog("session %s: rebuilt from uploaded config", s->cookie);
    return true;
}

static void complete_upload(WebClient *c) {
    Session *s = c->sess;
    bool ok = false;
    if (s) {
        if (c->upload_kind == WS_UPLOAD_DISK) {
            ok = mount_disk_bytes(s, c->upload_drive, c->upload, c->upload_len,
                                  c->upload_name);
        } else {
            char path[CONFIG_PATH_MAX];
            snprintf(path, sizeof(path), "%s/session.conf", s->upload_dir);
            FILE *f = fopen(path, "wb");
            if (f) {
                size_t w = fwrite(c->upload, 1, c->upload_len, f);
                fclose(f);
                if (w == c->upload_len) ok = rebuild_from_config_file(s, path);
            }
        }
    }
    free(c->upload);
    c->upload = NULL;
    c->upload_len = c->upload_cap = 0;
    c->state = WC_READ_REQUEST;
    respond_status(c, ok ? "204 No Content" : "500 Internal Server Error", true);
}

/* Small uploads (rare) that fit the ordinary request buffer. */
static void route_disk_small(Session *s, WebClient *c, const char *query,
                             const char *body, int body_len) {
    char dv[4], name[64] = "upload.dsk";
    if (!query_param(query, "drive", dv, sizeof(dv)) ||
        (dv[0] != '0' && dv[0] != '1') || dv[1] != '\0' || body_len <= 0) {
        respond_status(c, "400 Bad Request", true);
        return;
    }
    query_param(query, "name", name, sizeof(name));
    bool ok = mount_disk_bytes(s, dv[0] - '0', body, (size_t)body_len, name);
    respond_status(c, ok ? "204 No Content" : "500 Internal Server Error", true);
}

/* A 1984.conf is ordinarily well under WS_REQ_MAX, so this — not the
 * WC_READ_UPLOAD path in try_dispatch() — is the common case. */
static void route_config_small(Session *s, WebClient *c,
                               const char *body, int body_len) {
    if (body_len <= 0) {
        respond_status(c, "400 Bad Request", true);
        return;
    }
    char path[CONFIG_PATH_MAX];
    snprintf(path, sizeof(path), "%s/session.conf", s->upload_dir);
    bool ok = false;
    FILE *f = fopen(path, "wb");
    if (f) {
        size_t w = fwrite(body, 1, (size_t)body_len, f);
        fclose(f);
        if (w == (size_t)body_len) ok = rebuild_from_config_file(s, path);
    }
    respond_status(c, ok ? "204 No Content" : "500 Internal Server Error", true);
}

static void route_paste(Session *s, WebClient *c, const char *body, int body_len) {
    char text[WS_PASTE_MAX + 1];
    int n = 0;
    for (int i = 0; i < body_len && n < WS_PASTE_MAX; i++) {
        unsigned char ch = (unsigned char)body[i];
        if (ch >= 0x20 || ch == '\n' || ch == '\t')
            text[n++] = (char)ch;
    }
    text[n] = '\0';
    if (n > 0) paste_text(&s->paste, text);
    respond_status(c, "204 No Content", true);
}

/* Dispatch a fully-buffered request. `token` is the resolved cookie value
 * (may be freshly generated if this is a session-creating "/"). */
static void route(WebClient *c, const char *method, char *path,
                  const char *body, int body_len, const char *cookie_in) {
    char *query = strchr(path, '?');
    if (query) *query++ = '\0';
    else query = path + strlen(path);

    bool get  = strcmp(method, "GET") == 0;
    bool post = strcmp(method, "POST") == 0;
    if (!get && !post) {
        respond_status(c, "405 Method Not Allowed", false);
        return;
    }

    if (strcmp(path, "/") == 0 && get) {
        Session *s = session_lookup(cookie_in);
        if (!s) s = session_create(SDL_GetTicks());
        if (!s) { route_busy(c); return; }
        c->sess = s;
        route_page(c, s->cookie);
        return;
    }

    /* Every other endpoint needs an existing, live session. */
    Session *s = session_lookup(cookie_in);
    if (!s) {
        respond_status(c, "400 Bad Request", false);
        return;
    }
    c->sess = s;

    if (strcmp(path, "/stream") == 0 && get) route_stream(c);
    else if (strcmp(path, "/audio") == 0 && get) route_audio(c);
    else if (strcmp(path, "/status") == 0 && get) route_status(s, c);
    else if (strcmp(path, "/key") == 0)   route_key(s, c, query);
    else if (strcmp(path, "/joy") == 0)   route_joy(s, c, query);
    else if (strcmp(path, "/mouse") == 0) route_mouse(s, c, query);
    else if (strcmp(path, "/paste") == 0) route_paste(s, c, body, body_len);
    else if (strcmp(path, "/disk") == 0)  route_disk_small(s, c, query, body, body_len);
    else if (strcmp(path, "/session/config") == 0)
        route_config_small(s, c, body, body_len);
    else if (strcmp(path, "/reset") == 0) {
        cpc_reset(&s->cpc);
        respond_status(c, "204 No Content", true);
    } else respond_status(c, "404 Not Found", true);
}

static bool is_large_upload_target(const char *target, UploadKind *kind) {
    if (strncmp(target, "/disk?", 6) == 0 || strcmp(target, "/disk") == 0) {
        *kind = WS_UPLOAD_DISK;
        return true;
    }
    if (strncmp(target, "/session/config", 15) == 0 &&
        (target[15] == '\0' || target[15] == '?')) {
        *kind = WS_UPLOAD_CONFIG;
        return true;
    }
    return false;
}

static bool try_dispatch(WebClient *c) {
    c->req[c->req_len] = '\0';
    char *head_end = strstr(c->req, "\r\n\r\n");
    if (!head_end) {
        if (c->req_len >= WS_REQ_MAX - 1) {
            respond_status(c, "431 Request Header Fields Too Large", false);
            return true;
        }
        return false;
    }
    int head_len = (int)(head_end - c->req) + 4;

    c->req[head_len - 2] = '\0';
    int content_len = 0;
    char clv[16], conn[16], cookie_hdr[256], token[WS_TOKEN_LEN + 1] = "";
    if (header_value(c->req, "Content-Length", clv, sizeof(clv)))
        content_len = atoi(clv);
    bool asked_close = header_value(c->req, "Connection", conn, sizeof(conn)) &&
                       str_ieq(conn, "close");
    if (header_value(c->req, "Cookie", cookie_hdr, sizeof(cookie_hdr)))
        cookie_token(cookie_hdr, token, sizeof(token));
    c->req[head_len - 2] = '\r';

    char method[8] = "", target[512] = "";
    if (sscanf(c->req, "%7s %511s", method, target) != 2) {
        respond_status(c, "400 Bad Request", false);
        return true;
    }

    UploadKind kind;
    bool is_upload = strcmp(method, "POST") == 0 &&
                     is_large_upload_target(target, &kind);
    if (is_upload && content_len > WS_REQ_MAX - 1 - head_len) {
        if (content_len <= 0 || content_len > WS_UPLOAD_MAX) {
            respond_status(c, "413 Content Too Large", false);
            return true;
        }
        Session *s = session_lookup(token);
        if (!s) {
            respond_status(c, "400 Bad Request", false);
            return true;
        }
        char *query = strchr(target, '?');
        if (query) *query++ = '\0';
        else query = target + strlen(target);
        int drive = 0;
        if (kind == WS_UPLOAD_DISK) {
            char dv[4];
            if (!query_param(query, "drive", dv, sizeof(dv)) ||
                (dv[0] != '0' && dv[0] != '1') || dv[1] != '\0') {
                respond_status(c, "400 Bad Request", false);
                return true;
            }
            drive = dv[0] - '0';
        }
        uint8_t *buf = malloc((size_t)content_len);
        if (!buf) {
            respond_status(c, "500 Internal Server Error", false);
            return true;
        }
        int already = c->req_len - head_len;
        if (already > 0) memcpy(buf, c->req + head_len, (size_t)already);
        c->sess = s;
        c->upload = buf;
        c->upload_len = (size_t)already;
        c->upload_cap = (size_t)content_len;
        c->upload_kind = kind;
        c->upload_drive = drive;
        snprintf(c->upload_name, sizeof(c->upload_name), "upload");
        query_param(query, "name", c->upload_name, sizeof(c->upload_name));
        c->state = WC_READ_UPLOAD;
        c->req_len = 0;
        return true;
    }

    if (content_len < 0 || head_len + content_len > WS_REQ_MAX - 1) {
        respond_status(c, "413 Content Too Large", false);
        return true;
    }
    if (c->req_len < head_len + content_len)
        return false;

    route(c, method, target, c->req + head_len, content_len, token);
    if (asked_close && c->state != WC_STREAMING && c->state != WC_AUDIO_STREAM)
        c->close_after_send = true;

    int consumed = head_len + content_len;
    int leftover = c->req_len - consumed;
    if (leftover > 0 && c->state == WC_READ_REQUEST)
        memmove(c->req, c->req + consumed, (size_t)leftover);
    c->req_len = (c->state == WC_READ_REQUEST) ? leftover : 0;
    return true;
}

/* ---------- accept / poll / frame ---------- */

static void ws_poll(void) {
    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int fd = accept(g_listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) break;
        WebClient *c = NULL;
        for (int i = 0; i < WS_MAX_CLIENTS; i++)
            if (g_clients[i].state == WC_FREE) { c = &g_clients[i]; break; }
        if (!c) {
            static const char busy[] =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Connection: close\r\nContent-Length: 0\r\n\r\n";
            send(fd, busy, sizeof(busy) - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
            sock_close(fd);
            wlog("connection refused: all %d connection slots busy", WS_MAX_CLIENTS);
            continue;
        }
        sock_set_nonblocking(fd);
        c->fd = fd;
        if (!inet_ntop(AF_INET, &peer.sin_addr, c->ip, sizeof(c->ip)))
            snprintf(c->ip, sizeof(c->ip), "?");
        c->state = WC_READ_REQUEST;
    }

    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        WebClient *c = &g_clients[i];
        if (c->state == WC_FREE) continue;
        for (;;) {
            char tmp[1024];
            char *dst = tmp;
            int room = (int)sizeof(tmp);
            if (c->state == WC_READ_REQUEST) {
                dst = c->req + c->req_len;
                room = WS_REQ_MAX - 1 - c->req_len;
                if (room <= 0) break;
            } else if (c->state == WC_READ_UPLOAD) {
                dst = (char *)c->upload + c->upload_len;
                room = (int)(c->upload_cap - c->upload_len);
                if (room <= 0) break;
            }
            ssize_t n = recv(c->fd, dst, (size_t)room, MSG_DONTWAIT);
            if (n == 0 || (n < 0 && !sock_would_block())) {
                wc_close(c);
                break;
            }
            if (n < 0) break;
            if (c->state == WC_READ_REQUEST) {
                c->req_len += (int)n;
                while (c->state == WC_READ_REQUEST && c->req_len > 0 &&
                       !c->close_after_send && try_dispatch(c))
                    ;
            } else if (c->state == WC_READ_UPLOAD) {
                c->upload_len += (size_t)n;
                if (c->upload_len >= c->upload_cap)
                    complete_upload(c);
            }
        }
    }

    for (int i = 0; i < WS_MAX_CLIENTS; i++)
        if (g_clients[i].state != WC_FREE && g_clients[i].out_len > g_clients[i].out_off)
            wc_send(&g_clients[i]);
}

static void session_audio_frame(Session *s) {
    if (s->audio_len == 0) return;

    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        WebClient *c = &g_clients[i];
        if (c->sess != s || c->state != WC_AUDIO_STREAM ||
            c->out_len != c->out_off)
            continue;
        if (c->sent_audio_seq == s->audio_seq) continue;
        c->out_off = c->out_len = 0;
        if (!wc_out(c, s->audio_bytes, s->audio_len)) {
            wc_close(c);
            continue;
        }
        c->sent_audio_seq = s->audio_seq;
        wc_send(c);
    }
}

static void session_frame(Session *s, uint64_t now) {
    session_audio_frame(s);

    if ((s->decim ^= 1) != 0) return;   /* 25 fps cap */

    bool want = false;
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        WebClient *c = &g_clients[i];
        if (c->sess == s && c->state == WC_STREAMING && c->out_len == c->out_off)
            want = true;
    }
    if (!want) return;

    uint32_t hash = display_hash(&s->cpc.display);
    if (!s->last_gif || hash != s->last_hash ||
        now - s->last_encode_ms >= WS_KEEPALIVE_MS) {
        s->last_gif = gifcap_encode_single(s->enc, s->cpc.display.pixels, &s->last_gif_len);
        if (!s->last_gif) return;
        s->last_hash = hash;
        s->last_encode_ms = now;
    }

    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        WebClient *c = &g_clients[i];
        if (c->sess != s || c->state != WC_STREAMING || c->out_len != c->out_off) continue;
        if (c->sent_hash == s->last_hash && now - c->sent_ms < WS_KEEPALIVE_MS) continue;
        c->out_off = c->out_len = 0;
        wc_printf(c, "--" WS_BOUNDARY "\r\n"
                     "Content-Type: image/gif\r\n"
                     "Content-Length: %zu\r\n\r\n", s->last_gif_len);
        wc_out(c, s->last_gif, s->last_gif_len);
        wc_out(c, "\r\n", 2);
        c->sent_hash = s->last_hash;
        c->sent_ms = now;
        wc_send(c);
    }
}

/* ---------- entry point ---------- */

int websvc_run(int port) {
    if (port < 1 || port > 65535) {
        fprintf(stderr, "1984 websvc: invalid port %d\n", port);
        return 1;
    }

    signal(SIGINT, on_term_signal);
#ifdef SIGTERM
    signal(SIGTERM, on_term_signal);
#endif

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "offscreen", SDL_HINT_OVERRIDE);
    SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "1984 websvc: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "1984 websvc: socket failed\n");
        SDL_Quit();
        return 1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 8) < 0) {
        fprintf(stderr, "1984 websvc: cannot listen on port %d\n", port);
        sock_close(fd);
        SDL_Quit();
        return 1;
    }
    sock_set_nonblocking(fd);
    g_listen_fd = fd;

    wlog("emulator-as-a-service listening on 0.0.0.0:%d — no authentication, "
         "LAN-visible; up to %d concurrent sessions, %d min idle timeout",
         port, WS_MAX_SESSIONS, WS_IDLE_TIMEOUT_MS / 60000);
    wlog_urls(port);

    for (int i = 0; i < WS_MAX_CLIENTS; i++) g_clients[i].fd = -1;

    const uint64_t FRAME_NS = 20000000ULL;   /* 50 Hz, matches the classic loop */
    uint64_t next_frame = SDL_GetTicksNS();
    g_last_sweep_ms = SDL_GetTicks();

    while (g_running) {
        ws_poll();

        uint64_t now_ms = SDL_GetTicks();
        for (int i = 0; i < WS_MAX_SESSIONS; i++) {
            Session *s = &g_sessions[i];
            if (!s->in_use) continue;
            paste_tick(&s->paste, &s->cpc.kbd);
            cpc_frame(&s->cpc);
            session_frame(s, now_ms);
        }

        if (now_ms - g_last_sweep_ms >= WS_SWEEP_INTERVAL_MS) {
            session_idle_sweep(now_ms);
            g_last_sweep_ms = now_ms;
        }

        next_frame += FRAME_NS;
        uint64_t now_ns = SDL_GetTicksNS();
        if (now_ns < next_frame) SDL_DelayNS(next_frame - now_ns);
        else if (now_ns > next_frame + 3 * FRAME_NS) next_frame = now_ns;
    }

    wlog("shutting down");
    for (int i = 0; i < WS_MAX_CLIENTS; i++)
        if (g_clients[i].state != WC_FREE) wc_close(&g_clients[i]);
    for (int i = 0; i < WS_MAX_SESSIONS; i++)
        if (g_sessions[i].in_use) session_destroy(&g_sessions[i]);
    sock_close(g_listen_fd);
    SDL_Quit();
    return 0;
}

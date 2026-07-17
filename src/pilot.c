/* pilot.c — host-PTY "auto-pilot" input device. See pilot.h. */

#if defined(__NetBSD__)
#define _NETBSD_SOURCE 1
#endif

#define _XOPEN_SOURCE 600   /* posix_openpt, grantpt, unlockpt, ptsname */
#define _DEFAULT_SOURCE     /* cfmakeraw */
/* See usifac.c / issue #203: the BSDs hide cfmakeraw behind __BSD_VISIBLE,
 * which _XOPEN_SOURCE turns off. */
#if defined(__FreeBSD__) || defined(__DragonFly__) || defined(__OpenBSD__) || defined(__NetBSD__)
#define __BSD_VISIBLE 1
#endif

#include "pilot.h"
#include "cpc.h"
#include "mouse.h"
#include "ch376.h"
#include "kbd.h"
#include "notify.h"
#include "snapshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdarg.h>

/* CPC joystick 1 lives in keyboard matrix row 9, bits 0-5 (matches joy.c). */
#define JOY_ROW    9
#define JOY_UP     0
#define JOY_DOWN   1
#define JOY_LEFT   2
#define JOY_RIGHT  3
#define JOY_FIRE1  4
#define JOY_FIRE2  5

#define CLICK_HOLD_FRAMES 4   /* how long a `click` keeps a button pressed */
#define JOY_DEADZONE      0.5 /* magnitude below which the stick is neutral */

#ifndef _WIN32

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <errno.h>

/* ---- PTY setup (mirrors usifac.c / kbd_pty.c) ----------------------------- */

bool pilot_open(Pilot *p, const char *link, PilotTarget initial, bool reply_stderr) {
    memset(p, 0, sizeof(*p));
    p->fd     = -1;
    p->target = initial;
    p->reply_stderr = reply_stderr;
    p->wait_timeout_frame = -1;
    p->keytap_scancode = -1;
    p->last_pixels = calloc((size_t)CPC_SCREEN_W * CPC_SCREEN_H, sizeof(u32));

    int fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) { perror("pilot: posix_openpt"); return false; }
    if (grantpt(fd) < 0 || unlockpt(fd) < 0) {
        perror("pilot: grantpt/unlockpt"); close(fd); return false;
    }
    const char *name = ptsname(fd);
    if (!name) { close(fd); return false; }
    snprintf(p->slave, sizeof(p->slave), "%s", name);

    /* Open the slave once in raw mode so the /dev/pts node persists across
     * host reconnects and carries no line-discipline cooking. See the long
     * note in usifac.c open_pty() for why this is done on the slave fd. */
    int sfd = open(name, O_RDWR | O_NOCTTY);
    if (sfd >= 0) {
        struct termios tio;
        if (tcgetattr(sfd, &tio) == 0) {
            cfmakeraw(&tio);
            tio.c_cflag |= CS8 | CREAD | CLOCAL;
            tcsetattr(sfd, TCSANOW, &tio);
        }
        close(sfd);
    }

    p->fd = fd;

    if (link && link[0]) {
        unlink(link);
        if (symlink(p->slave, link) == 0) {
            snprintf(p->link, sizeof(p->link), "%s", link);
        } else {
            fprintf(stderr, "pilot: symlink(%s -> %s) failed: %s\n",
                    link, p->slave, strerror(errno));
        }
    }

    if (p->link[0])
        notify_post("Pilot: PTY ready at %s (alias %s)", p->slave, p->link);
    else
        notify_post("Pilot: PTY ready at %s", p->slave);
    fprintf(stderr, "1984: pilot PTY: %s (target=%s)\n",
            p->slave, initial == PILOT_JOY ? "joystick" : "mouse");
    return true;
}

bool pilot_is_open(const Pilot *p) { return p->fd >= 0; }

/* ---- command parsing ------------------------------------------------------ */

/* Set the held velocity from a polar (magnitude, angle°). THETA uses the
 * conventional maths orientation (0=right, 90=up, counter-clockwise); the
 * mouse uses screen coordinates where +y points down, hence the negated y. */
static void set_vector(Pilot *p, double mag, double ang_deg) {
    p->mag = mag;
    p->ang = ang_deg;
    double r = ang_deg * (M_PI / 180.0);
    p->vx =  mag * cos(r);
    p->vy = -mag * sin(r);
}

static void release_all_buttons(Pilot *p) {
    for (int b = 0; b < 3; b++) { p->btn[b] = false; p->click_left[b] = 0; }
}

static void release_keytap(Pilot *p, CPC *cpc) {
    if (p->keytap_left > 0 && p->keytap_scancode >= 0)
        kbd_sdl_key(&cpc->kbd, (SDL_Scancode)p->keytap_scancode, false);
    p->keytap_left = 0;
    p->keytap_scancode = -1;
}

static int parse_timeout(char *const *tok, int n, int idx, int frame_now) {
    if (idx < n) {
        int frames = atoi(tok[idx]);
        if (frames >= 0) return frame_now + frames;
    }
    return -1;
}

static void start_wait(Pilot *p, PilotWaitMode mode, int frame_now, int timeout_frame) {
    p->wait_mode = mode;
    p->wait_start_frame = frame_now;
    p->wait_timeout_frame = timeout_frame;
}

static void pilot_reply(Pilot *p, const char *fmt, ...) {
    char buf[256];
    char line[256];
    va_list ap;
    int n;

    if (p->fd < 0) return;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    snprintf(line, sizeof(line), "%s", buf);
    if (n >= (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 2;
    buf[n++] = '\n';
    (void)!write(p->fd, buf, (size_t)n);
    if (p->reply_stderr) {
        fprintf(stderr, "1984: pilot reply: %s\n", line);
        fflush(stderr);
    }
}

static void reply_state(Pilot *p, CPC *cpc) {
    pilot_reply(p,
                "state target=%s mag=%.3f ang=%.3f move_left=%d "
                "buttons=%d%d%d click_left=%d,%d,%d joy_held=%d "
                "frame=%d hash=%08X changed=%d,%d,%d,%d quiet=%d "
                "mouse_symbiface=%d mouse_albireo=%d",
                p->target == PILOT_JOY ? "joy" : "mouse",
                p->mag, p->ang, p->move_left,
                p->btn[0] ? 1 : 0, p->btn[1] ? 1 : 0, p->btn[2] ? 1 : 0,
                p->click_left[0], p->click_left[1], p->click_left[2],
                p->joy_held ? 1 : 0, p->frame_count, (unsigned)p->fb_hash,
                p->changed_x, p->changed_y, p->changed_w, p->changed_h, p->quiet_frames,
                cpc->symbiface_mouse ? 1 : 0, cpc->albireo_mouse ? 1 : 0);
}

static SDL_Scancode scancode_from_token(const char *name) {
    SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
    char buf[64];
    size_t j = 0;
    if (!name || !name[0]) return SDL_SCANCODE_UNKNOWN;
    sc = SDL_GetScancodeFromName(name);
    if (sc != SDL_SCANCODE_UNKNOWN) return sc;
    for (size_t i = 0; name[i] && j + 1 < sizeof(buf); i++) {
        buf[j++] = (name[i] == '_') ? ' ' : name[i];
    }
    buf[j] = '\0';
    return SDL_GetScancodeFromName(buf);
}

static void reply_changed(Pilot *p) {
    int count = (p->changed_w > 0 && p->changed_h > 0) ? 1 : 0;
    pilot_reply(p, "changed count=%d rect=%d,%d,%d,%d quiet=%d",
                count, p->changed_x, p->changed_y, p->changed_w, p->changed_h,
                p->quiet_frames);
}

static void finish_wait(Pilot *p, const char *reason) {
    pilot_reply(p, "ok wait reason=%s frame=%d hash=%08X changed=%d,%d,%d,%d quiet=%d",
                reason, p->frame_count, (unsigned)p->fb_hash,
                p->changed_x, p->changed_y, p->changed_w, p->changed_h,
                p->quiet_frames);
    p->wait_mode = PILOT_WAIT_NONE;
    p->wait_timeout_frame = -1;
}

static void fail_wait(Pilot *p, const char *reason) {
    pilot_reply(p, "err wait reason=%s frame=%d hash=%08X",
                reason, p->frame_count, (unsigned)p->fb_hash);
    p->wait_mode = PILOT_WAIT_NONE;
    p->wait_timeout_frame = -1;
}

static void exec_line(Pilot *p, CPC *cpc, Display *d, Paste *paste, char *line) {
    /* Strip comments and surrounding whitespace, normalise commas to spaces. */
    char *hash = strchr(line, '#');
    char raw[256];
    int frame_now = p->frame_count;
    if (hash) *hash = '\0';
    snprintf(raw, sizeof(raw), "%s", line);
    for (char *c = line; *c; c++) if (*c == ',') *c = ' ';

    char *tok[8]; int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(line, " \t\r\n", &save);
         t && n < 8; t = strtok_r(NULL, " \t\r\n", &save))
        tok[n++] = t;
    if (n == 0) return;

    char *cmd = tok[0];
    for (char *c = cmd; *c; c++) *c = (char)tolower((unsigned char)*c);

    /* Bare "R THETA" (first token is a number) == implicit move. */
    if (isdigit((unsigned char)cmd[0]) ||
        ((cmd[0] == '-' || cmd[0] == '+' || cmd[0] == '.') && cmd[1])) {
        if (n >= 2) {
            set_vector(p, atof(tok[0]), atof(tok[1]));
            p->move_left = 0;
            pilot_reply(p, "ok move mag=%.3f ang=%.3f", p->mag, p->ang);
        } else {
            pilot_reply(p, "err move needs R THETA");
        }
        return;
    }

    if (!strcmp(cmd, "move") || !strcmp(cmd, "m") || !strcmp(cmd, "v")) {
        if (n >= 3) {
            set_vector(p, atof(tok[1]), atof(tok[2]));
            p->move_left = 0;
            pilot_reply(p, "ok move mag=%.3f ang=%.3f", p->mag, p->ang);
        } else {
            pilot_reply(p, "err move needs R THETA");
        }
    } else if (!strcmp(cmd, "hold")) {
        if (n >= 4) {
            p->move_left = atoi(tok[1]);
            if (p->move_left < 0) p->move_left = 0;
            set_vector(p, atof(tok[2]), atof(tok[3]));
            pilot_reply(p, "ok hold frames=%d mag=%.3f ang=%.3f",
                        p->move_left, p->mag, p->ang);
        } else {
            pilot_reply(p, "err hold needs FRAMES R THETA");
        }
    } else if (!strcmp(cmd, "stop") || !strcmp(cmd, "s") ||
               !strcmp(cmd, "halt") || !strcmp(cmd, "x")) {
        set_vector(p, 0.0, p->ang);
        p->move_left = 0;
        pilot_reply(p, "ok stop");
    } else if (!strcmp(cmd, "press") || !strcmp(cmd, "p")) {
        if (n >= 2) {
            int b = atoi(tok[1]) - 1;
            if (b >= 0 && b < 3) {
                p->btn[b] = true; p->click_left[b] = 0;
                pilot_reply(p, "ok press button=%d", b + 1);
            } else {
                pilot_reply(p, "err invalid button");
            }
        } else {
            pilot_reply(p, "err press needs BUTTON");
        }
    } else if (!strcmp(cmd, "release") || !strcmp(cmd, "u")) {
        if (n >= 2) {
            int b = atoi(tok[1]) - 1;
            if (b >= 0 && b < 3) {
                p->btn[b] = false; p->click_left[b] = 0;
                pilot_reply(p, "ok release button=%d", b + 1);
            } else {
                pilot_reply(p, "err invalid button");
            }
        } else {
            pilot_reply(p, "err release needs BUTTON");
        }
    } else if (!strcmp(cmd, "click") || !strcmp(cmd, "c")) {
        if (n >= 2) {
            int b = atoi(tok[1]) - 1;
            if (b >= 0 && b < 3) {
                p->btn[b] = true; p->click_left[b] = CLICK_HOLD_FRAMES;
                pilot_reply(p, "ok click button=%d frames=%d", b + 1, CLICK_HOLD_FRAMES);
            } else {
                pilot_reply(p, "err invalid button");
            }
        } else {
            pilot_reply(p, "err click needs BUTTON");
        }
    } else if (!strcmp(cmd, "hold-click") || !strcmp(cmd, "holdclick")) {
        if (n >= 3) {
            int frames = atoi(tok[1]);
            int b = atoi(tok[2]) - 1;
            if (frames < 0) frames = 0;
            if (b >= 0 && b < 3) {
                p->btn[b] = true; p->click_left[b] = frames;
                pilot_reply(p, "ok hold-click button=%d frames=%d", b + 1, frames);
            } else {
                pilot_reply(p, "err invalid button");
            }
        } else {
            pilot_reply(p, "err hold-click needs FRAMES BUTTON");
        }
    } else if (!strcmp(cmd, "scroll")) {
        if (n >= 2) {
            p->scroll_pending += atoi(tok[1]);
            pilot_reply(p, "ok scroll pending=%d", p->scroll_pending);
        } else {
            pilot_reply(p, "err scroll needs DELTA");
        }
    } else if (!strcmp(cmd, "target") || !strcmp(cmd, "t")) {
        if (n >= 2) {
            char k = (char)tolower((unsigned char)tok[1][0]);
            p->target = (k == 'j') ? PILOT_JOY : PILOT_MOUSE;
            pilot_reply(p, "ok target=%s", p->target == PILOT_JOY ? "joy" : "mouse");
        } else {
            pilot_reply(p, "err target needs mouse|joy");
        }
    } else if (!strcmp(cmd, "reset")) {
        set_vector(p, 0.0, 0.0);
        p->move_left = 0;
        release_all_buttons(p);
        release_keytap(p, cpc);
        pilot_reply(p, "ok reset");
    } else if (!strcmp(cmd, "state")) {
        reply_state(p, cpc);
    } else if (!strcmp(cmd, "frame")) {
        pilot_reply(p, "frame value=%d", p->frame_count);
    } else if (!strcmp(cmd, "hash")) {
        pilot_reply(p, "hash value=%08X", (unsigned)p->fb_hash);
    } else if (!strcmp(cmd, "changed")) {
        reply_changed(p);
    } else if (!strcmp(cmd, "key-down")) {
        if (n >= 2) {
            SDL_Scancode sc = scancode_from_token(tok[1]);
            if (sc != SDL_SCANCODE_UNKNOWN && kbd_sdl_key(&cpc->kbd, sc, true))
                pilot_reply(p, "ok key-down name=%s", tok[1]);
            else
                pilot_reply(p, "err key-down unknown=%s", tok[1]);
        } else {
            pilot_reply(p, "err key-down needs NAME");
        }
    } else if (!strcmp(cmd, "key-up")) {
        if (n >= 2) {
            SDL_Scancode sc = scancode_from_token(tok[1]);
            if (sc != SDL_SCANCODE_UNKNOWN && kbd_sdl_key(&cpc->kbd, sc, false))
                pilot_reply(p, "ok key-up name=%s", tok[1]);
            else
                pilot_reply(p, "err key-up unknown=%s", tok[1]);
        } else {
            pilot_reply(p, "err key-up needs NAME");
        }
    } else if (!strcmp(cmd, "key-tap")) {
        if (n >= 3) {
            SDL_Scancode sc = scancode_from_token(tok[1]);
            int frames = atoi(tok[2]);
            if (frames < 1) frames = 1;
            if (sc != SDL_SCANCODE_UNKNOWN && kbd_sdl_key(&cpc->kbd, sc, true)) {
                release_keytap(p, cpc);
                p->keytap_scancode = (int)sc;
                p->keytap_left = frames;
                pilot_reply(p, "ok key-tap name=%s frames=%d", tok[1], frames);
            } else {
                pilot_reply(p, "err key-tap unknown=%s", tok[1]);
            }
        } else {
            pilot_reply(p, "err key-tap needs NAME FRAMES");
        }
    } else if (!strcmp(cmd, "paste")) {
        char *text = raw;
        while (*text && !isspace((unsigned char)*text)) text++;
        while (*text && isspace((unsigned char)*text)) text++;
        paste_text_raw(paste, text);
        pilot_reply(p, "ok paste len=%d", (int)strlen(text));
    } else if (!strcmp(cmd, "snapshot-save")) {
        if (n >= 2) {
            if (snapshot_save(cpc, tok[1]) == 0) pilot_reply(p, "ok snapshot-save path=%s", tok[1]);
            else pilot_reply(p, "err snapshot-save path=%s", tok[1]);
        } else {
            pilot_reply(p, "err snapshot-save needs PATH");
        }
    } else if (!strcmp(cmd, "snapshot-load")) {
        if (n >= 2) {
            if (snapshot_load(cpc, tok[1]) == 0) {
                p->have_last_pixels = false;
                p->changed_w = p->changed_h = 0;
                p->quiet_frames = 0;
                pilot_reply(p, "ok snapshot-load path=%s", tok[1]);
            } else pilot_reply(p, "err snapshot-load path=%s", tok[1]);
        } else {
            pilot_reply(p, "err snapshot-load needs PATH");
        }
    } else if (!strcmp(cmd, "crop")) {
        if (n >= 6) {
            int x = atoi(tok[2]), y = atoi(tok[3]), w = atoi(tok[4]), h = atoi(tok[5]);
            int scale = (n >= 7) ? atoi(tok[6]) : 1;
            if (display_save_crop_ppm(d, tok[1], x, y, w, h, scale)) {
                pilot_reply(p, "ok crop path=%s rect=%d,%d,%d,%d scale=%d",
                            tok[1], x, y, w, h, scale < 1 ? 1 : scale);
            } else {
                pilot_reply(p, "err crop path=%s rect=%d,%d,%d,%d scale=%d",
                            tok[1], x, y, w, h, scale < 1 ? 1 : scale);
            }
        } else {
            pilot_reply(p, "err crop needs PATH X Y W H [SCALE]");
        }
    } else if (!strcmp(cmd, "wait")) {
        if (n < 2) {
            pilot_reply(p, "err wait needs MODE");
        } else if (!strcmp(tok[1], "frames")) {
            if (n >= 3) {
                p->wait_target_frames = atoi(tok[2]);
                if (p->wait_target_frames < 0) p->wait_target_frames = 0;
                start_wait(p, PILOT_WAIT_FRAMES, frame_now, parse_timeout(tok, n, 3, frame_now));
            } else pilot_reply(p, "err wait frames needs N [TIMEOUT]");
        } else if (!strcmp(tok[1], "hash-eq")) {
            if (n >= 3) {
                p->wait_hash = (u32)strtoul(tok[2], NULL, 16);
                start_wait(p, PILOT_WAIT_HASH_EQ, frame_now, parse_timeout(tok, n, 3, frame_now));
            } else pilot_reply(p, "err wait hash-eq needs HEX [TIMEOUT]");
        } else if (!strcmp(tok[1], "hash-ne")) {
            if (n >= 3) {
                p->wait_hash = (u32)strtoul(tok[2], NULL, 16);
                start_wait(p, PILOT_WAIT_HASH_NE, frame_now, parse_timeout(tok, n, 3, frame_now));
            } else pilot_reply(p, "err wait hash-ne needs HEX [TIMEOUT]");
        } else if (!strcmp(tok[1], "change")) {
            start_wait(p, PILOT_WAIT_CHANGE, frame_now, parse_timeout(tok, n, 2, frame_now));
        } else if (!strcmp(tok[1], "quiet")) {
            if (n >= 3) {
                p->wait_quiet_need = atoi(tok[2]);
                if (p->wait_quiet_need < 1) p->wait_quiet_need = 1;
                start_wait(p, PILOT_WAIT_QUIET, frame_now, parse_timeout(tok, n, 3, frame_now));
            } else pilot_reply(p, "err wait quiet needs N [TIMEOUT]");
        } else {
            pilot_reply(p, "err wait unknown=%s", tok[1]);
        }
    } else {
        fprintf(stderr, "[pilot] unknown command: %s\n", cmd);
        pilot_reply(p, "err unknown command=%s", cmd);
    }
}

/* ---- per-frame application ------------------------------------------------ */

/* The 8-way direction mask for a given angle, or 0 when below the deadzone. */
static unsigned char joy_mask_for(double mag, double ang_deg) {
    if (mag < JOY_DEADZONE) return 0;
    double a = fmod(ang_deg, 360.0);
    if (a < 0) a += 360.0;
    int sector = ((int)floor(a / 45.0 + 0.5)) & 7;   /* 0=E,1=NE,2=N,...,7=SE */
    static const unsigned char m[8] = {
        1u << JOY_RIGHT,                       /* E  */
        (1u << JOY_RIGHT) | (1u << JOY_UP),    /* NE */
        1u << JOY_UP,                          /* N  */
        (1u << JOY_LEFT)  | (1u << JOY_UP),    /* NW */
        1u << JOY_LEFT,                        /* W  */
        (1u << JOY_LEFT)  | (1u << JOY_DOWN),  /* SW */
        1u << JOY_DOWN,                        /* S  */
        (1u << JOY_RIGHT) | (1u << JOY_DOWN),  /* SE */
    };
    return m[sector];
}

void pilot_tick(Pilot *p, CPC *cpc, Display *d, Paste *paste) {
    if (p->fd < 0) return;

    /* Drain everything available into the line buffer, executing on newline. */
    char c;
    while (p->wait_mode == PILOT_WAIT_NONE && read(p->fd, &c, 1) == 1) {
        if (c == '\n' || c == '\r') {
            p->line[p->line_len] = '\0';
            exec_line(p, cpc, d, paste, p->line);
            p->line_len = 0;
        } else if (p->line_len < (int)sizeof(p->line) - 1) {
            p->line[p->line_len++] = c;
        }
        /* Overlong lines past the buffer are silently truncated at newline. */
    }

    /* Auto-release expired clicks. */
    for (int b = 0; b < 3; b++)
        if (p->click_left[b] > 0 && --p->click_left[b] == 0)
            p->btn[b] = false;
    if (p->keytap_left > 0 && --p->keytap_left == 0)
        release_keytap(p, cpc);

    if (p->target == PILOT_MOUSE) {
        if (p->joy_held) {   /* just switched away from joystick — let go of row 9 */
            for (int col = JOY_UP; col <= JOY_FIRE2; col++)
                kbd_key_up(&cpc->kbd, JOY_ROW, col);
            p->joy_held = false;
        }

        /* Accumulate sub-pixel velocity, emit the integer part this frame. */
        p->acc_x += p->vx;
        p->acc_y += p->vy;
        int dx = (int)p->acc_x, dy = (int)p->acc_y;
        p->acc_x -= dx;
        p->acc_y -= dy;
        if (dx || dy) {
            if (cpc->symbiface_mouse) mouse_move(&cpc->mouse, dx, dy);
            if (cpc->albireo_mouse)   ch376_mouse_move(&cpc->ch376, dx, dy);
        }
        for (int b = 0; b < 3; b++) {
            if (cpc->symbiface_mouse) mouse_button(&cpc->mouse, b, p->btn[b]);
            if (cpc->albireo_mouse)   ch376_mouse_button(&cpc->ch376, b, p->btn[b]);
        }
        if (p->scroll_pending) {
            if (cpc->symbiface_mouse) mouse_scroll(&cpc->mouse, p->scroll_pending);
            p->scroll_pending = 0;
        }
    } else { /* PILOT_JOY */
        unsigned char mask = joy_mask_for(p->mag, p->ang);
        if (p->btn[0]) mask |= 1u << JOY_FIRE1;   /* button 1 -> Fire1 */
        if (p->btn[1]) mask |= 1u << JOY_FIRE2;   /* button 2 -> Fire2 */
        for (int col = JOY_UP; col <= JOY_FIRE2; col++) {
            if (mask & (1u << col)) kbd_key_down(&cpc->kbd, JOY_ROW, col);
            else                    kbd_key_up  (&cpc->kbd, JOY_ROW, col);
        }
        p->joy_held = true;
    }

    if (p->move_left > 0 && --p->move_left == 0)
        set_vector(p, 0.0, p->ang);
}

void pilot_post_frame(Pilot *p, CPC *cpc, Display *d, int frame_count) {
    bool changed;
    if (p->fd < 0) return;

    p->frame_count = frame_count;
    p->fb_hash = display_hash(d);
    if (!p->last_pixels) return;
    if (!p->have_last_pixels) {
        display_copy_visible(d, p->last_pixels);
        p->have_last_pixels = true;
        p->changed_x = p->changed_y = 0;
        p->changed_w = p->changed_h = 0;
        p->quiet_frames = 0;
        return;
    }

    changed = display_changed_rect(d, p->last_pixels,
                                   &p->changed_x, &p->changed_y,
                                   &p->changed_w, &p->changed_h);
    if (changed) p->quiet_frames = 0;
    else p->quiet_frames++;

    if (p->wait_mode == PILOT_WAIT_NONE) return;
    if (p->wait_timeout_frame >= 0 && frame_count >= p->wait_timeout_frame) {
        fail_wait(p, "timeout");
        return;
    }

    switch (p->wait_mode) {
    case PILOT_WAIT_FRAMES:
        if (frame_count - p->wait_start_frame >= p->wait_target_frames)
            finish_wait(p, "frames");
        break;
    case PILOT_WAIT_HASH_EQ:
        if (p->fb_hash == p->wait_hash) finish_wait(p, "hash-eq");
        break;
    case PILOT_WAIT_HASH_NE:
        if (p->fb_hash != p->wait_hash) finish_wait(p, "hash-ne");
        break;
    case PILOT_WAIT_CHANGE:
        if (changed) finish_wait(p, "change");
        break;
    case PILOT_WAIT_QUIET:
        if (p->quiet_frames >= p->wait_quiet_need) finish_wait(p, "quiet");
        break;
    case PILOT_WAIT_NONE:
    default:
        break;
    }
    (void)cpc;
}

#else  /* _WIN32 — no PTYs */

bool pilot_open(Pilot *p, const char *link, PilotTarget initial, bool reply_stderr) {
    (void)link; (void)initial; (void)reply_stderr;
    memset(p, 0, sizeof(*p)); p->fd = -1;
    fprintf(stderr, "1984: --pilot is not supported on Windows\n");
    return false;
}
bool pilot_is_open(const Pilot *p) { (void)p; return false; }
void pilot_tick(Pilot *p, CPC *cpc, Display *d, Paste *paste) {
    (void)p; (void)cpc; (void)d; (void)paste;
}
void pilot_post_frame(Pilot *p, CPC *cpc, Display *d, int frame_count) {
    (void)p; (void)cpc; (void)d; (void)frame_count;
}

#endif

/* pilot.c — host-PTY "auto-pilot" input device. See pilot.h. */

#define _XOPEN_SOURCE 600   /* posix_openpt, grantpt, unlockpt, ptsname */
#define _DEFAULT_SOURCE     /* cfmakeraw */

#include "pilot.h"
#include "cpc.h"
#include "mouse.h"
#include "ch376.h"
#include "kbd.h"
#include "notify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

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

bool pilot_open(Pilot *p, const char *link, PilotTarget initial) {
    memset(p, 0, sizeof(*p));
    p->fd     = -1;
    p->target = initial;

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

static void exec_line(Pilot *p, char *line) {
    /* Strip comments and surrounding whitespace, normalise commas to spaces. */
    char *hash = strchr(line, '#');
    if (hash) *hash = '\0';
    for (char *c = line; *c; c++) if (*c == ',') *c = ' ';

    char *tok[4]; int n = 0;
    char *save = NULL;
    for (char *t = strtok_r(line, " \t\r\n", &save);
         t && n < 4; t = strtok_r(NULL, " \t\r\n", &save))
        tok[n++] = t;
    if (n == 0) return;

    char *cmd = tok[0];
    for (char *c = cmd; *c; c++) *c = (char)tolower((unsigned char)*c);

    /* Bare "R THETA" (first token is a number) == implicit move. */
    if (isdigit((unsigned char)cmd[0]) ||
        ((cmd[0] == '-' || cmd[0] == '+' || cmd[0] == '.') && cmd[1])) {
        if (n >= 2) set_vector(p, atof(tok[0]), atof(tok[1]));
        return;
    }

    if (!strcmp(cmd, "move") || !strcmp(cmd, "m") || !strcmp(cmd, "v")) {
        if (n >= 3) set_vector(p, atof(tok[1]), atof(tok[2]));
    } else if (!strcmp(cmd, "stop") || !strcmp(cmd, "s") ||
               !strcmp(cmd, "halt") || !strcmp(cmd, "x")) {
        set_vector(p, 0.0, p->ang);
    } else if (!strcmp(cmd, "press") || !strcmp(cmd, "p")) {
        if (n >= 2) { int b = atoi(tok[1]) - 1; if (b >= 0 && b < 3) { p->btn[b] = true;  p->click_left[b] = 0; } }
    } else if (!strcmp(cmd, "release") || !strcmp(cmd, "u")) {
        if (n >= 2) { int b = atoi(tok[1]) - 1; if (b >= 0 && b < 3) { p->btn[b] = false; p->click_left[b] = 0; } }
    } else if (!strcmp(cmd, "click") || !strcmp(cmd, "c")) {
        if (n >= 2) { int b = atoi(tok[1]) - 1; if (b >= 0 && b < 3) { p->btn[b] = true;  p->click_left[b] = CLICK_HOLD_FRAMES; } }
    } else if (!strcmp(cmd, "scroll")) {
        if (n >= 2) p->scroll_pending += atoi(tok[1]);
    } else if (!strcmp(cmd, "target") || !strcmp(cmd, "t")) {
        if (n >= 2) {
            char k = (char)tolower((unsigned char)tok[1][0]);
            p->target = (k == 'j') ? PILOT_JOY : PILOT_MOUSE;
        }
    } else if (!strcmp(cmd, "reset")) {
        set_vector(p, 0.0, 0.0);
        release_all_buttons(p);
    } else {
        fprintf(stderr, "[pilot] unknown command: %s\n", cmd);
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

void pilot_tick(Pilot *p, CPC *cpc) {
    if (p->fd < 0) return;

    /* Drain everything available into the line buffer, executing on newline. */
    char c;
    while (read(p->fd, &c, 1) == 1) {
        if (c == '\n' || c == '\r') {
            p->line[p->line_len] = '\0';
            exec_line(p, p->line);
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
}

#else  /* _WIN32 — no PTYs */

bool pilot_open(Pilot *p, const char *link, PilotTarget initial) {
    (void)link; (void)initial;
    memset(p, 0, sizeof(*p)); p->fd = -1;
    fprintf(stderr, "1984: --pilot is not supported on Windows\n");
    return false;
}
bool pilot_is_open(const Pilot *p) { (void)p; return false; }
void pilot_tick(Pilot *p, CPC *cpc) { (void)p; (void)cpc; }

#endif

/* kbd_pty — bidirectional terminal-like PTY for the CPC. Chars written
 * to the PTY get injected into the keyboard matrix via the existing
 * paste machinery; chars the CPC writes via the firmware text-out
 * vector (&BB5A: TXT WR CHAR — covers BASIC, AMSDOS, and CP/M+
 * command-line output) are streamed out the PTY. Used by the issue
 * #129 investigation harness so external scripts can drive the
 * machine end-to-end without xdotool/Xvfb. */

#define _XOPEN_SOURCE 600
#include "kbd_pty.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PTYs are POSIX-only — Windows stubs return failure so callers fail
 * closed if --kbd-pty is passed. The harness is Linux-only anyway. */
#ifndef _WIN32

#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

static int   s_fd = -1;
static char  s_slave[256];

const char *kbd_pty_open(void) {
    if (s_fd >= 0) return s_slave;

    int fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (fd < 0) return NULL;
    if (grantpt(fd) < 0 || unlockpt(fd) < 0) { close(fd); return NULL; }

    const char *name = ptsname(fd);
    if (!name) { close(fd); return NULL; }
    strncpy(s_slave, name, sizeof(s_slave) - 1);
    s_slave[sizeof(s_slave) - 1] = '\0';

    /* Raw, non-blocking — no echo, no canonical line buffering, no
     * signal interpretation, no input translation. */
    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    tio.c_cflag = CS8 | CREAD | CLOCAL;
    tio.c_iflag = 0;
    tio.c_oflag = 0;
    tio.c_lflag = 0;       /* clears ECHO, ICANON, ISIG */
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;
    cfsetispeed(&tio, B9600);
    cfsetospeed(&tio, B9600);
    tcsetattr(fd, TCSANOW, &tio);
    fcntl(fd, F_SETFL, O_NONBLOCK);

    s_fd = fd;
    return s_slave;
}

bool kbd_pty_is_open(void) { return s_fd >= 0; }

/* In-memory line buffer of pending PTY input that paste hasn't picked up
 * yet. Drained into the paste buf each tick. */
static char s_in[512];
static int  s_in_len;

void kbd_pty_tick(Paste *p) {
    if (s_fd < 0) return;

    /* Drain anything new on the PTY into our local buffer. */
    while (s_in_len < (int)sizeof(s_in) - 1) {
        char c;
        ssize_t n = read(s_fd, &c, 1);
        if (n <= 0) break;
        /* paste.c maps '\n' to Enter and silently skips '\r'. Translate
         * CR-only or CRLF coming over the wire into LF so terminal-style
         * line endings ("\r") fire Enter correctly. */
        if (c == '\r') c = '\n';
        s_in[s_in_len++] = c;
        if (getenv("ONE_K_TRACE_KBDPTY")) {
            fprintf(stderr, "[kbd_pty] read byte 0x%02X (%c)\n",
                    (unsigned)c, (c >= 32 && c < 127) ? c : '.');
            fflush(stderr);
        }
    }
    s_in[s_in_len] = '\0';

    /* If the existing paste is still typing, leave its buffer alone —
     * we'll catch up on the next tick. */
    if (p->buf && p->pos < p->len) return;
    if (s_in_len == 0) return;

    /* Hand the buffered chars to paste. paste_text appends a newline at
     * the end; to avoid that on partial input we strip a trailing
     * delimiter if we already have one. The harness sends chars + \n
     * explicitly anyway, so the auto-newline isn't a problem in
     * practice — paste skips characters not in its keymap. */
    paste_text_raw(p, s_in);
    s_in_len = 0;
}

/* Called by cpc.c whenever the Z80 enters the firmware text-out vector
 * &BB5A. The byte to print is in A. We pipe it straight through; the
 * far side gets raw CPC bytes including the firmware's CR/LF. */
void kbd_pty_emit_char(unsigned char c) {
    if (s_fd < 0) return;
    /* Best-effort write; don't block if the reader isn't draining. */
    (void)!write(s_fd, &c, 1);
}

void kbd_pty_emit_buf(const void *buf, int len) {
    if (s_fd < 0 || len <= 0) return;
    (void)!write(s_fd, buf, (size_t)len);
}

#else  /* _WIN32 */

const char *kbd_pty_open(void) { return NULL; }
bool        kbd_pty_is_open(void) { return false; }
void        kbd_pty_tick(Paste *p) { (void)p; }
void        kbd_pty_emit_char(unsigned char c) { (void)c; }
void        kbd_pty_emit_buf(const void *buf, int len) { (void)buf; (void)len; }

#endif  /* _WIN32 */

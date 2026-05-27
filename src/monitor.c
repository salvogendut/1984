#define _XOPEN_SOURCE 600   /* posix_openpt, grantpt, unlockpt, ptsname */
#include "monitor.h"
#include "z80dis.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
/* PTY / serial support */
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

/* ---- Layout ---- */
#define MON_COLS     80
#define MON_ROWS     25
#define OUT_ROWS     (MON_ROWS - 1)   /* rows 0..23 = output; row 24 = input */
#define CHAR_W       8
#define CHAR_H       8
/* Non-uniform scale: 12 × 28.8 px chars → 960 × 720 window (4:3) */
#define FONT_SCALE_X  1.5f
#define FONT_SCALE_Y  3.6f
#define WIN_W  ((int)(MON_COLS * CHAR_W * FONT_SCALE_X))   /* 960 */
#define WIN_H  ((int)(MON_ROWS * CHAR_H * FONT_SCALE_Y))   /* 720 */

/* ---- Colours ---- */
#define C_BG      0x00, 0x00, 0x00
#define C_TEXT    0xCC, 0xFF, 0xCC   /* green phosphor */
#define C_MORE    0xFF, 0xFF, 0x00
#define C_PROMPT  0x88, 0xFF, 0x88

/* ---- Paging mode ---- */
typedef enum { PAGE_NONE, PAGE_DIS, PAGE_HEX } PageMode;

/* ---- Streaming state structs ---- */
typedef struct {
    bool active;
    u16  addr, end_addr;
    bool has_end;
    int  lines_left;
    u8   snap[65536];
} DisState;

typedef struct {
    bool active;
    u32  addr, end_addr;
    bool has_end;
} HexState;

/* ---- Hex dump format
 *  >AAAAA XX XX XX XX XX XX XX XX: CCCCCCCC
 *  col 0     '>'
 *  col 1-5   5-digit address
 *  col 6     ' '
 *  col 7-29  8 hex bytes (23 chars)
 *  col 30    ':'   col 31  ' '
 *  col 32-39 8 ASCII chars  ← reverse video
 */
#define HEX_REV_COL  32
#define HEX_BYTES    8

/* ---- PTY state ---- */
typedef struct {
    int  fd;          /* master fd, -1 when closed */
    char slave[64];   /* slave device path */
    char inbuf[256];  /* line accumulation */
    int  inlen;
} MonPty;

/* ---- Monitor struct ---- */
struct Monitor {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    bool          open;

    char  screen[OUT_ROWS][MON_COLS + 1];
    int   screen_rev[OUT_ROWS];   /* 0 = normal; >0 = reverse-video column */

    char  input[MON_COLS + 1];
    int   input_len;

    PageMode  page_mode;
    int       page_lines;
    DisState  dis;
    HexState  hex;

    MonPty    pty;
    Mem      *mem;
};

/* ---- PTY output helpers (defined before screen helpers that call them) ---- */

static void pty_write(Monitor *mon, const char *s, int len) {
    if (mon->pty.fd < 0) return;
    while (len > 0) {
        ssize_t n = write(mon->pty.fd, s, (size_t)len);
        if (n <= 0) break;
        s += n; len -= (int)n;
    }
}

static void pty_puts_line(Monitor *mon, const char *line, int rev_col) {
    if (mon->pty.fd < 0) return;
    int len = (int)strlen(line);
    /* Trim padding spaces added to fill the screen buffer */
    while (len > 0 && line[len - 1] == ' ') len--;

    if (rev_col > 0 && rev_col < len) {
        pty_write(mon, line, rev_col);
        pty_write(mon, "\033[7m", 4);          /* reverse video on  */
        pty_write(mon, line + rev_col, len - rev_col);
        pty_write(mon, "\033[0m", 4);           /* reverse video off */
    } else {
        pty_write(mon, line, len);
    }
    pty_write(mon, "\r\n", 2);
}

static void pty_more_prompt(Monitor *mon) {
    if (mon->pty.fd < 0) return;
    pty_write(mon, "-- more -- (ENTER/SPACE to continue) ", 37);
}

static void pty_ready_prompt(Monitor *mon) {
    if (mon->pty.fd < 0) return;
    pty_write(mon, ">_ ", 3);
}

/* ---- Screen helpers ---- */

static void screen_scroll(Monitor *mon) {
    memmove(mon->screen[0], mon->screen[1],
            (OUT_ROWS - 1) * (MON_COLS + 1));
    memmove(&mon->screen_rev[0], &mon->screen_rev[1],
            (OUT_ROWS - 1) * sizeof(int));
    memset(mon->screen[OUT_ROWS - 1], ' ', MON_COLS);
    mon->screen[OUT_ROWS - 1][MON_COLS] = '\0';
    mon->screen_rev[OUT_ROWS - 1] = 0;
}

static void screen_puts_ex(Monitor *mon, const char *line, int rev_col) {
    screen_scroll(mon);
    int len = (int)strlen(line);
    if (len > MON_COLS) len = MON_COLS;
    memcpy(mon->screen[OUT_ROWS - 1], line, (size_t)len);
    for (int i = len; i < MON_COLS; i++)
        mon->screen[OUT_ROWS - 1][i] = ' ';
    mon->screen[OUT_ROWS - 1][MON_COLS] = '\0';
    mon->screen_rev[OUT_ROWS - 1] = rev_col;
    pty_puts_line(mon, line, rev_col);
}

static void screen_puts(Monitor *mon, const char *line) {
    screen_puts_ex(mon, line, 0);
}

/* ---- Disassembly streaming ---- */

static bool dis_emit_line(Monitor *mon) {
    if (!mon->dis.active) return true;
    if (mon->dis.has_end &&
        (u16)(mon->dis.addr - mon->dis.end_addr) < 0x8000) {
        mon->dis.active = false; return true;
    }
    if (!mon->dis.has_end && mon->dis.lines_left <= 0) {
        mon->dis.active = false; return true;
    }

    u16  addr = mon->dis.addr;
    char mnem[64];
    int  bytes = z80dis(mon->dis.snap, addr, mnem, sizeof(mnem));
    if (bytes <= 0) bytes = 1;

    char hexbuf[16] = "";
    int  show = bytes > 4 ? 4 : bytes;
    for (int i = 0; i < show; i++) {
        char tmp[4];
        snprintf(tmp, sizeof(tmp), "%02X ", mon->dis.snap[(u16)(addr + i)]);
        strncat(hexbuf, tmp, sizeof(hexbuf) - strlen(hexbuf) - 1);
    }
    char line[MON_COLS + 32];
    snprintf(line, sizeof(line), "%04X  %-12s %s", addr, hexbuf, mnem);
    screen_puts(mon, line);

    mon->dis.addr = (u16)(addr + bytes);
    if (!mon->dis.has_end) mon->dis.lines_left--;
    mon->page_lines++;
    if (mon->page_lines >= OUT_ROWS - 1 && mon->dis.active) return false;
    return true;
}

static void dis_run_page(Monitor *mon) {
    mon->page_lines = 0;
    while (mon->dis.active) {
        if (!dis_emit_line(mon)) {
            mon->page_mode = PAGE_DIS;
            pty_more_prompt(mon);
            return;
        }
    }
    mon->page_mode = PAGE_NONE;
}

/* ---- Hex dump streaming ---- */

static u8 hex_read(Monitor *mon, u32 addr) {
    return (addr < (u32)mon->mem->ram_size) ? mon->mem->ram[addr] : 0xFF;
}

static bool hex_emit_line(Monitor *mon) {
    if (!mon->hex.active) return true;
    if (mon->hex.has_end && mon->hex.addr > mon->hex.end_addr) {
        mon->hex.active = false; return true;
    }

    u32 addr  = mon->hex.addr;
    int count = HEX_BYTES;
    if (mon->hex.has_end && addr + (u32)count - 1 > mon->hex.end_addr)
        count = (int)(mon->hex.end_addr - addr + 1);

    char hexpart[HEX_BYTES * 3 + 1] = "";
    for (int i = 0; i < HEX_BYTES; i++) {
        char tmp[4];
        if (i < count) snprintf(tmp, sizeof(tmp), "%02X ", hex_read(mon, addr + (u32)i));
        else           snprintf(tmp, sizeof(tmp), "   ");
        strncat(hexpart, tmp, sizeof(hexpart) - strlen(hexpart) - 1);
    }
    int hlen = (int)strlen(hexpart);
    if (hlen > 0 && hexpart[hlen - 1] == ' ') hexpart[hlen - 1] = '\0';

    char ascpart[HEX_BYTES + 1];
    for (int i = 0; i < HEX_BYTES; i++) {
        u8 b = (i < count) ? hex_read(mon, addr + (u32)i) : ' ';
        ascpart[i] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
    }
    ascpart[HEX_BYTES] = '\0';

    char line[MON_COLS + 32];
    snprintf(line, sizeof(line), ">%05X %s: %s", addr, hexpart, ascpart);
    screen_puts_ex(mon, line, HEX_REV_COL);

    mon->hex.addr += HEX_BYTES;
    mon->page_lines++;
    if (mon->page_lines >= OUT_ROWS - 1 && mon->hex.active) return false;
    return true;
}

static void hex_run_page(Monitor *mon) {
    mon->page_lines = 0;
    while (mon->hex.active) {
        if (!hex_emit_line(mon)) {
            mon->page_mode = PAGE_HEX;
            pty_more_prompt(mon);
            return;
        }
    }
    mon->page_mode = PAGE_NONE;
}

/* ---- Command execution ---- */

static void mon_exec(Monitor *mon, const char *raw) {
    char echo[MON_COLS + 4];
    snprintf(echo, sizeof(echo), "> %s", raw);
    screen_puts(mon, echo);

    while (*raw == ' ') raw++;
    if (*raw == '\0') return;

    char cmd = (char)toupper((unsigned char)*raw);
    const char *args = raw + 1;
    while (*args == ' ') args++;

    switch (cmd) {
    case 'D': {
        unsigned a1 = 0, a2 = 0;
        int n = sscanf(args, "%x %x", &a1, &a2);
        if (n < 1) { screen_puts(mon, "Usage: D <addr> [<end_addr>]"); break; }
        for (int i = 0; i < 65536; i++)
            mon->dis.snap[i] = mem_read(mon->mem, (u16)i);
        mon->dis.addr       = (u16)a1;
        mon->dis.has_end    = (n >= 2);
        mon->dis.end_addr   = (u16)a2;
        mon->dis.lines_left = 10;
        mon->dis.active     = true;
        dis_run_page(mon);
        break;
    }
    case 'M': {
        unsigned a1 = 0, a2 = 0;
        int n = sscanf(args, "%x %x", &a1, &a2);
        if (n < 1) { screen_puts(mon, "Usage: M <addr> [<end_addr>]"); break; }
        mon->hex.addr    = a1;
        mon->hex.has_end = (n >= 2);
        mon->hex.end_addr = a2;
        if (!mon->hex.has_end) {
            mon->hex.has_end  = true;
            mon->hex.end_addr = a1 + (u32)(OUT_ROWS - 2) * HEX_BYTES - 1;
        }
        mon->hex.active = true;
        hex_run_page(mon);
        break;
    }
    case 'X':
    case 'Q':
        mon->open = false;
        SDL_StopTextInput(mon->window);
        SDL_HideWindow(mon->window);
        break;
    default:
        screen_puts(mon, "Commands:  D <addr> [<end>]   M <addr> [<end>]   X = exit");
        break;
    }
}

/* ---- SDL input handling ---- */

static void handle_keydown(Monitor *mon, SDL_Keycode key, const char *text) {
    if (mon->page_mode != PAGE_NONE) {
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
            mon->page_lines = 0;
            if (mon->page_mode == PAGE_DIS)      dis_run_page(mon);
            else if (mon->page_mode == PAGE_HEX) hex_run_page(mon);
        }
        return;
    }
    if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
        mon->input[mon->input_len] = '\0';
        mon_exec(mon, mon->input);
        mon->input_len = 0;
        mon->input[0]  = '\0';
    } else if (key == SDLK_BACKSPACE) {
        if (mon->input_len > 0) mon->input[--mon->input_len] = '\0';
    } else if (text && *text >= 0x20) {
        int tlen = (int)strlen(text);
        if (mon->input_len + tlen < MON_COLS - 4) {
            memcpy(mon->input + mon->input_len, text, (size_t)tlen);
            mon->input_len += tlen;
            mon->input[mon->input_len] = '\0';
        }
    }
}

/* ---- Rendering ---- */

static void draw_line(SDL_Renderer *r, float y, const char *line, int rev_col) {
    int len = (int)strlen(line);

    if (rev_col <= 0 || rev_col >= len) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(r, C_TEXT, 255);
        SDL_RenderDebugText(r, 0, y, line);
        return;
    }
    /* Normal part */
    char tmp[MON_COLS + 1];
    memcpy(tmp, line, (size_t)rev_col);
    tmp[rev_col] = '\0';
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, C_TEXT, 255);
    SDL_RenderDebugText(r, 0, y, tmp);
    /* Green background rect */
    int   rlen = len - rev_col;
    float rx   = (float)(rev_col * CHAR_W);
    SDL_FRect bg = { rx, y, (float)(rlen * CHAR_W), (float)CHAR_H };
    SDL_SetRenderDrawColor(r, C_TEXT, 255);
    SDL_RenderFillRect(r, &bg);
    /* Reversed chars in black */
    SDL_SetRenderDrawColor(r, C_BG, 255);
    SDL_RenderDebugText(r, rx, y, line + rev_col);
}

void monitor_render(Monitor *mon) {
    if (!mon->open) return;

    SDL_SetRenderDrawColor(mon->renderer, C_BG, 255);
    SDL_RenderClear(mon->renderer);
    SDL_SetRenderScale(mon->renderer, FONT_SCALE_X, FONT_SCALE_Y);

    for (int row = 0; row < OUT_ROWS; row++)
        draw_line(mon->renderer, (float)(row * CHAR_H),
                  mon->screen[row], mon->screen_rev[row]);

    float input_y = (float)(OUT_ROWS * CHAR_H);
    if (mon->page_mode != PAGE_NONE) {
        SDL_SetRenderDrawBlendMode(mon->renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(mon->renderer, C_MORE, 255);
        SDL_RenderDebugText(mon->renderer, 0, input_y,
                            "-- more -- (ENTER/SPACE to continue)");
    } else {
        SDL_SetRenderDrawBlendMode(mon->renderer, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(mon->renderer, C_PROMPT, 255);
        SDL_RenderDebugText(mon->renderer, 0, input_y, ">_");
        if (mon->input_len > 0) {
            SDL_SetRenderDrawColor(mon->renderer, C_TEXT, 255);
            SDL_RenderDebugText(mon->renderer,
                                (float)(3 * CHAR_W), input_y, mon->input);
        }
        float cx = (float)((3 + mon->input_len) * CHAR_W);
        SDL_SetRenderDrawColor(mon->renderer, C_TEXT, 255);
        SDL_FRect cur = { cx, input_y, CHAR_W, CHAR_H };
        SDL_RenderFillRect(mon->renderer, &cur);
    }

    SDL_SetRenderScale(mon->renderer, 1.0f, 1.0f);
    SDL_RenderPresent(mon->renderer);
}

/* ---- Public API ---- */

Monitor *monitor_create(Mem *mem) {
    Monitor *mon = calloc(1, sizeof(*mon));
    if (!mon) return NULL;
    mon->mem    = mem;
    mon->open   = false;
    mon->pty.fd = -1;

    mon->window = SDL_CreateWindow("Memory Monitor", WIN_W, WIN_H,
                                   SDL_WINDOW_RESIZABLE);
    if (!mon->window) { free(mon); return NULL; }

    mon->renderer = SDL_CreateRenderer(mon->window, NULL);
    if (!mon->renderer) {
        SDL_DestroyWindow(mon->window);
        free(mon);
        return NULL;
    }

    SDL_HideWindow(mon->window);

    screen_puts(mon, "CPC Memory Monitor");
    screen_puts(mon, "  D <addr> [<end>]  disassemble Z80 (10 lines default)");
    screen_puts(mon, "  M <addr> [<end>]  hex+ASCII dump (page default)");
    screen_puts(mon, "  X                 close monitor");
    screen_puts(mon, "  Addresses: 4-digit hex (CPU) or 5-digit hex (physical RAM)");
    screen_puts(mon, "");

    return mon;
}

void monitor_destroy(Monitor *mon) {
    if (!mon) return;
    if (mon->pty.fd >= 0) { close(mon->pty.fd); mon->pty.fd = -1; }
    if (mon->renderer) SDL_DestroyRenderer(mon->renderer);
    if (mon->window)   SDL_DestroyWindow(mon->window);
    free(mon);
}

bool monitor_is_open(const Monitor *mon) { return mon && mon->open; }

SDL_WindowID monitor_window_id(const Monitor *mon) {
    return mon ? SDL_GetWindowID(mon->window) : 0;
}

void monitor_open(Monitor *mon) {
    if (!mon) return;
    mon->open = true;
    SDL_ShowWindow(mon->window);
    SDL_RaiseWindow(mon->window);
    SDL_StartTextInput(mon->window);
}

bool monitor_handle_event(Monitor *mon, SDL_Event *e) {
    if (!mon) return false;

    if (e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
        e->window.windowID == SDL_GetWindowID(mon->window)) {
        mon->open = false;
        SDL_StopTextInput(mon->window);
        SDL_HideWindow(mon->window);
        return true;
    }
    if (e->type == SDL_EVENT_KEY_DOWN &&
        e->key.windowID == SDL_GetWindowID(mon->window)) {
        handle_keydown(mon, e->key.key, NULL);
        return true;
    }
    if (e->type == SDL_EVENT_TEXT_INPUT &&
        e->text.windowID == SDL_GetWindowID(mon->window)) {
        handle_keydown(mon, 0, e->text.text);
        return true;
    }
    return false;
}

/* ---- PTY public API ---- */

const char *monitor_pty_open(Monitor *mon) {
    if (!mon || mon->pty.fd >= 0) return NULL;

    int fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (fd < 0) return NULL;
    if (grantpt(fd) < 0 || unlockpt(fd) < 0) { close(fd); return NULL; }

    const char *name = ptsname(fd);
    if (!name) { close(fd); return NULL; }
    strncpy(mon->pty.slave, name, sizeof(mon->pty.slave) - 1);

    /* Raw mode at 9600 baud — PTYs don't enforce rate but minicom needs to match */
    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    tio.c_cflag  = CS8 | CREAD | CLOCAL;
    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 0;
    cfsetispeed(&tio, B9600);
    cfsetospeed(&tio, B9600);
    tcsetattr(fd, TCSANOW, &tio);

    /* Non-blocking reads so the main loop doesn't stall */
    fcntl(fd, F_SETFL, O_NONBLOCK);

    mon->pty.fd = fd;

    /* Send welcome banner to anyone already connected */
    pty_puts_line(mon, "CPC Memory Monitor (serial PTY)", 0);
    pty_puts_line(mon, "  D <addr> [<end>]  disassemble Z80", 0);
    pty_puts_line(mon, "  M <addr> [<end>]  hex+ASCII dump", 0);
    pty_puts_line(mon, "  X                 close SDL window", 0);
    pty_write(mon, ">_ ", 3);

    return mon->pty.slave;
}

void monitor_pty_tick(Monitor *mon) {
    if (!mon || mon->pty.fd < 0) return;

    u8      buf[64];
    ssize_t n;
    while ((n = read(mon->pty.fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            u8 ch = buf[i];

            /* While paging, any ENTER/SPACE/key continues output */
            if (mon->page_mode != PAGE_NONE) {
                if (ch == '\r' || ch == '\n' || ch == ' ') {
                    pty_write(mon, "\r\n", 2);
                    mon->page_lines = 0;
                    if (mon->page_mode == PAGE_DIS)      dis_run_page(mon);
                    else if (mon->page_mode == PAGE_HEX) hex_run_page(mon);
                    if (mon->page_mode == PAGE_NONE) pty_ready_prompt(mon);
                }
                continue;
            }

            if (ch == '\r' || ch == '\n') {
                mon->pty.inbuf[mon->pty.inlen] = '\0';
                pty_write(mon, "\r\n", 2);
                mon_exec(mon, mon->pty.inbuf);
                mon->pty.inlen = 0;
                if (mon->page_mode == PAGE_NONE) pty_ready_prompt(mon);
            } else if ((ch == 0x08 || ch == 0x7F) && mon->pty.inlen > 0) {
                mon->pty.inlen--;
                pty_write(mon, "\x08 \x08", 3);   /* erase last char */
            } else if (ch >= 0x20 && ch < 0x7F &&
                       mon->pty.inlen < (int)sizeof(mon->pty.inbuf) - 1) {
                mon->pty.inbuf[mon->pty.inlen++] = (char)ch;
                pty_write(mon, (char *)&ch, 1);    /* echo */
            }
        }
    }
}

#include "monitor.h"
#include "z80dis.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---- Layout ---- */
#define MON_COLS     80
#define MON_ROWS     25
#define OUT_ROWS     (MON_ROWS - 1)   /* rows 0..23 = output; row 24 = input */
#define CHAR_W       8
#define CHAR_H       8
#define FONT_SCALE   1.5f
#define WIN_W        ((int)(MON_COLS * CHAR_W * FONT_SCALE))   /* 960 */
#define WIN_H        ((int)(MON_ROWS * CHAR_H * FONT_SCALE))   /* 300 */

/* ---- Colours ---- */
#define C_BG      0x00, 0x00, 0x00
#define C_TEXT    0xCC, 0xFF, 0xCC   /* green phosphor */
#define C_MORE    0xFF, 0xFF, 0x00   /* "-- more --" in yellow */
#define C_PROMPT  0x88, 0xFF, 0x88

/* ---- Paging mode ---- */
typedef enum { PAGE_NONE, PAGE_DIS, PAGE_HEX } PageMode;

/* ---- Disassembly streaming state ---- */
typedef struct {
    bool  active;
    u16   addr;
    u16   end_addr;
    bool  has_end;
    int   lines_left;
    u8    snap[65536];
} DisState;

/* ---- Hex dump streaming state ---- */
typedef struct {
    bool  active;
    u32   addr;
    u32   end_addr;
    bool  has_end;
} HexState;

/* ---- Hex dump format constants ----
 *  >AAAAA XX XX XX XX XX XX XX XX: CCCCCCCC
 *  col 0      = '>'
 *  col 1-5    = 5-digit address
 *  col 6      = ' '
 *  col 7-29   = 8 hex bytes "XX " × 7 + "XX" (23 chars)
 *  col 30     = ':'
 *  col 31     = ' '
 *  col 32-39  = 8 ASCII chars  ← reverse video starts here
 */
#define HEX_REV_COL  32
#define HEX_BYTES    8

struct Monitor {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    bool          open;

    /* Output screen buffer */
    char  screen[OUT_ROWS][MON_COLS + 1];
    int   screen_rev[OUT_ROWS];  /* 0 = normal; >0 = column where reverse video starts */

    /* Input line */
    char  input[MON_COLS + 1];
    int   input_len;

    /* Paging */
    PageMode  page_mode;
    int       page_lines;
    DisState  dis;
    HexState  hex;

    Mem *mem;
};

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
}

static void screen_puts(Monitor *mon, const char *line) {
    screen_puts_ex(mon, line, 0);
}

/* ---- Disassembly streaming ---- */

static bool dis_emit_line(Monitor *mon) {
    if (!mon->dis.active) return true;
    if (mon->dis.has_end &&
        (u16)(mon->dis.addr - mon->dis.end_addr) < 0x8000) {
        mon->dis.active = false;
        return true;
    }
    if (!mon->dis.has_end && mon->dis.lines_left <= 0) {
        mon->dis.active = false;
        return true;
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

    if (mon->page_lines >= OUT_ROWS - 1 && mon->dis.active)
        return false;
    return true;
}

static void dis_run_page(Monitor *mon) {
    mon->page_lines = 0;
    while (mon->dis.active) {
        if (!dis_emit_line(mon)) {
            mon->page_mode = PAGE_DIS;
            return;
        }
    }
    mon->page_mode = PAGE_NONE;
}

/* ---- Hex dump streaming ---- */

static u8 hex_read(Monitor *mon, u32 addr) {
    if (addr < (u32)mon->mem->ram_size)
        return mon->mem->ram[addr];
    return 0xFF;
}

static bool hex_emit_line(Monitor *mon) {
    if (!mon->hex.active) return true;
    if (mon->hex.has_end && mon->hex.addr > mon->hex.end_addr) {
        mon->hex.active = false;
        return true;
    }

    u32 addr = mon->hex.addr;

    /* Determine how many bytes to show on this line */
    int count = HEX_BYTES;
    if (mon->hex.has_end && addr + (u32)count - 1 > mon->hex.end_addr)
        count = (int)(mon->hex.end_addr - addr + 1);

    /* Build hex portion */
    char hexpart[HEX_BYTES * 3 + 1] = "";
    for (int i = 0; i < HEX_BYTES; i++) {
        char tmp[4];
        if (i < count)
            snprintf(tmp, sizeof(tmp), "%02X ", hex_read(mon, addr + (u32)i));
        else
            snprintf(tmp, sizeof(tmp), "   ");  /* pad if short line */
        strncat(hexpart, tmp, sizeof(hexpart) - strlen(hexpart) - 1);
    }
    /* Remove trailing space after last byte, replace with colon-space */
    int hlen = (int)strlen(hexpart);
    if (hlen > 0 && hexpart[hlen - 1] == ' ') hexpart[hlen - 1] = '\0';

    /* Build ASCII portion (non-printable → '.') */
    char ascpart[HEX_BYTES + 1];
    for (int i = 0; i < HEX_BYTES; i++) {
        u8 b = (i < count) ? hex_read(mon, addr + (u32)i) : ' ';
        ascpart[i] = (b >= 0x20 && b < 0x7F) ? (char)b : '.';
    }
    ascpart[HEX_BYTES] = '\0';

    char line[MON_COLS + 32];
    snprintf(line, sizeof(line), ">%05X %s: %s",
             addr, hexpart, ascpart);
    screen_puts_ex(mon, line, HEX_REV_COL);

    mon->hex.addr += (u32)HEX_BYTES;
    mon->page_lines++;

    if (mon->page_lines >= OUT_ROWS - 1 && mon->hex.active)
        return false;
    return true;
}

static void hex_run_page(Monitor *mon) {
    mon->page_lines = 0;
    while (mon->hex.active) {
        if (!hex_emit_line(mon)) {
            mon->page_mode = PAGE_HEX;
            return;
        }
    }
    mon->page_mode = PAGE_NONE;
}

/* ---- Command execution ---- */

static void mon_puts(Monitor *mon, const char *s) {
    screen_puts(mon, s);
}

static void cmd_disassemble(Monitor *mon, const char *args) {
    unsigned a1 = 0, a2 = 0;
    int n = sscanf(args, "%x %x", &a1, &a2);
    if (n < 1) { mon_puts(mon, "Usage: D <addr> [<end_addr>]"); return; }

    for (int i = 0; i < 65536; i++)
        mon->dis.snap[i] = mem_read(mon->mem, (u16)i);

    mon->dis.addr       = (u16)a1;
    mon->dis.has_end    = (n >= 2);
    mon->dis.end_addr   = (u16)a2;
    mon->dis.lines_left = 10;
    mon->dis.active     = true;
    dis_run_page(mon);
}

static void cmd_hexdump(Monitor *mon, const char *args) {
    unsigned a1 = 0, a2 = 0;
    int n = sscanf(args, "%x %x", &a1, &a2);
    if (n < 1) { mon_puts(mon, "Usage: M <addr> [<end_addr>]"); return; }

    mon->hex.addr     = a1;
    mon->hex.has_end  = (n >= 2);
    mon->hex.end_addr = a2;
    mon->hex.active   = true;

    /* No end address: fill one page (OUT_ROWS-1 lines = (OUT_ROWS-1)*8 bytes) */
    if (!mon->hex.has_end) {
        mon->hex.has_end  = true;
        mon->hex.end_addr = a1 + (u32)(OUT_ROWS - 2) * HEX_BYTES - 1;
    }

    hex_run_page(mon);
}

static void mon_exec(Monitor *mon, const char *raw) {
    char echo[MON_COLS + 4];
    snprintf(echo, sizeof(echo), "> %s", raw);
    mon_puts(mon, echo);

    while (*raw == ' ') raw++;
    if (*raw == '\0') return;

    char cmd = (char)toupper((unsigned char)*raw);
    const char *args = raw + 1;
    while (*args == ' ') args++;

    switch (cmd) {
    case 'D': cmd_disassemble(mon, args); break;
    case 'M': cmd_hexdump(mon, args);     break;
    case 'X':
    case 'Q': mon->open = false;          break;
    default:
        mon_puts(mon, "Commands:  D <addr> [<end>]   M <addr> [<end>]   X = exit");
        break;
    }
}

/* ---- Input handling ---- */

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

/* Draw a line; if rev_col > 0, the portion from rev_col onward is shown
 * in reverse video (green background, black text). */
static void draw_line(SDL_Renderer *r, float y, const char *line, int rev_col) {
    int len = (int)strlen(line);

    if (rev_col <= 0 || rev_col >= len) {
        SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(r, C_TEXT, 255);
        SDL_RenderDebugText(r, 0, y, line);
        return;
    }

    /* Normal part (before reverse section) */
    char tmp[MON_COLS + 1];
    memcpy(tmp, line, (size_t)rev_col);
    tmp[rev_col] = '\0';
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, C_TEXT, 255);
    SDL_RenderDebugText(r, 0, y, tmp);

    /* Green background rect behind the reversed chars */
    int  rlen = len - rev_col;
    float rx  = (float)(rev_col * CHAR_W);
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
    SDL_SetRenderScale(mon->renderer, FONT_SCALE, FONT_SCALE);

    for (int row = 0; row < OUT_ROWS; row++) {
        float y = (float)(row * CHAR_H);
        draw_line(mon->renderer, y, mon->screen[row], mon->screen_rev[row]);
    }

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
        /* Cursor block */
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
    mon->mem  = mem;
    mon->open = false;

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

    mon_puts(mon, "CPC Memory Monitor");
    mon_puts(mon, "  D <addr> [<end>]  disassemble Z80 (10 lines default)");
    mon_puts(mon, "  M <addr> [<end>]  hex+ASCII dump (page default)");
    mon_puts(mon, "  X                 close monitor");
    mon_puts(mon, "  Addresses: 4-digit hex (CPU) or 5-digit hex (physical RAM)");
    mon_puts(mon, "");

    return mon;
}

void monitor_destroy(Monitor *mon) {
    if (!mon) return;
    if (mon->renderer) SDL_DestroyRenderer(mon->renderer);
    if (mon->window)   SDL_DestroyWindow(mon->window);
    free(mon);
}

bool monitor_is_open(const Monitor *mon) {
    return mon && mon->open;
}

SDL_WindowID monitor_window_id(const Monitor *mon) {
    return mon ? SDL_GetWindowID(mon->window) : 0;
}

void monitor_open(Monitor *mon) {
    if (!mon) return;
    mon->open = true;
    SDL_ShowWindow(mon->window);
    SDL_RaiseWindow(mon->window);
}

bool monitor_handle_event(Monitor *mon, SDL_Event *e) {
    if (!mon) return false;

    if (e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
        e->window.windowID == SDL_GetWindowID(mon->window)) {
        mon->open = false;
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

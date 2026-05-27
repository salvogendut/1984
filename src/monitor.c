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
#define C_BG         0x00, 0x00, 0x00
#define C_TEXT       0xCC, 0xFF, 0xCC   /* green phosphor */
#define C_DIM        0x44, 0x88, 0x44
#define C_MORE       0xFF, 0xFF, 0x00   /* "-- more --" in yellow */
#define C_PROMPT     0x88, 0xFF, 0x88
#define C_ADDR       0xAA, 0xCC, 0xFF
#define C_BYTES      0x77, 0x99, 0xBB
#define C_MNEM       0xCC, 0xFF, 0xCC

/* ---- Paging / streaming disassembly state ---- */
typedef struct {
    bool  active;
    u16   addr;        /* next address to disassemble */
    u16   end_addr;
    bool  has_end;
    int   lines_left;  /* for no-end case (max 10) */
    u8    snap[65536]; /* memory snapshot taken at command start */
} DisState;

struct Monitor {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    bool          open;

    /* Output screen buffer — OUT_ROWS lines of text */
    char          screen[OUT_ROWS][MON_COLS + 1];

    /* Input line */
    char          input[MON_COLS + 1];
    int           input_len;

    /* Paging */
    bool          waiting_more;   /* showing "-- more --", blocking input */
    int           page_lines;     /* output lines emitted this page */
    DisState      dis;

    Mem          *mem;
};

/* ---- Screen helpers ---- */

static void screen_scroll(Monitor *mon) {
    memmove(mon->screen[0], mon->screen[1],
            (OUT_ROWS - 1) * (MON_COLS + 1));
    memset(mon->screen[OUT_ROWS - 1], ' ', MON_COLS);
    mon->screen[OUT_ROWS - 1][MON_COLS] = '\0';
}

static void screen_puts(Monitor *mon, const char *line) {
    screen_scroll(mon);
    int len = (int)strlen(line);
    if (len > MON_COLS) len = MON_COLS;
    memcpy(mon->screen[OUT_ROWS - 1], line, (size_t)len);
    mon->screen[OUT_ROWS - 1][len] = '\0';
    /* pad with spaces */
    for (int i = len; i < MON_COLS; i++)
        mon->screen[OUT_ROWS - 1][i] = ' ';
    mon->screen[OUT_ROWS - 1][MON_COLS] = '\0';
}

/* ---- Disassembly streaming ---- */

/* Emit one disassembly line; returns false if we should pause for paging. */
static bool dis_emit_line(Monitor *mon) {
    if (!mon->dis.active) return true;
    if (mon->dis.has_end && (u16)(mon->dis.addr - mon->dis.end_addr) < 0x8000) {
        /* past end */
        mon->dis.active = false;
        return true;
    }
    if (!mon->dis.has_end && mon->dis.lines_left <= 0) {
        mon->dis.active = false;
        return true;
    }

    u16 addr = mon->dis.addr;
    char mnem[64];
    int  bytes = z80dis(mon->dis.snap, addr, mnem, sizeof(mnem));
    if (bytes <= 0) bytes = 1;

    /* Build hex bytes string (up to 4 bytes shown) */
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

    /* Pause when output area is full (leave 1 row for "-- more --") */
    if (mon->page_lines >= OUT_ROWS - 1 && mon->dis.active) {
        return false;  /* caller should show "-- more --" */
    }
    return true;
}

static void dis_run_page(Monitor *mon) {
    mon->page_lines = 0;
    while (mon->dis.active) {
        if (!dis_emit_line(mon)) {
            mon->waiting_more = true;
            return;
        }
    }
    mon->waiting_more = false;
}

/* ---- Command execution ---- */

static void mon_puts(Monitor *mon, const char *s) {
    screen_puts(mon, s);
}

static void cmd_disassemble(Monitor *mon, const char *args) {
    /* Parse: D <hex1> [<hex2>] */
    unsigned a1 = 0, a2 = 0;
    int n = sscanf(args, "%x %x", &a1, &a2);
    if (n < 1) { mon_puts(mon, "Usage: D <addr> [<end_addr>]"); return; }

    /* Snapshot current CPU-visible memory */
    for (int i = 0; i < 65536; i++)
        mon->dis.snap[i] = mem_read(mon->mem, (u16)i);

    mon->dis.addr     = (u16)a1;
    mon->dis.has_end  = (n >= 2);
    mon->dis.end_addr = (u16)a2;
    mon->dis.lines_left = 10;
    mon->dis.active   = true;

    dis_run_page(mon);
}

static void mon_exec(Monitor *mon, const char *raw) {
    /* Echo the command */
    char echo[MON_COLS + 4];
    snprintf(echo, sizeof(echo), "> %s", raw);
    mon_puts(mon, echo);

    /* Strip leading whitespace */
    while (*raw == ' ') raw++;
    if (*raw == '\0') return;

    char cmd = (char)toupper((unsigned char)*raw);
    const char *args = raw + 1;
    while (*args == ' ') args++;

    switch (cmd) {
    case 'D':
        cmd_disassemble(mon, args);
        break;
    case 'X':
    case 'Q':
        mon->open = false;
        break;
    default:
        mon_puts(mon, "Commands:  D <addr> [<end>]   X = exit");
        break;
    }
}

/* ---- Input handling ---- */

static void handle_keydown(Monitor *mon, SDL_Keycode key, const char *text) {
    if (mon->waiting_more) {
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE) {
            mon->page_lines = 0;
            dis_run_page(mon);
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
        /* Printable text input */
        int tlen = (int)strlen(text);
        if (mon->input_len + tlen < MON_COLS - 4) {
            memcpy(mon->input + mon->input_len, text, (size_t)tlen);
            mon->input_len += tlen;
            mon->input[mon->input_len] = '\0';
        }
    }
}

/* ---- Rendering ---- */

static void set_color(SDL_Renderer *r, Uint8 R, Uint8 G, Uint8 B) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, R, G, B, 255);
}

static void draw_text(SDL_Renderer *r, float x, float y, const char *s,
                      Uint8 R, Uint8 G, Uint8 B) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, R, G, B, 255);
    SDL_RenderDebugText(r, x, y, s);
}

void monitor_render(Monitor *mon) {
    if (!mon->open) return;

    SDL_SetRenderDrawColor(mon->renderer, C_BG, 255);
    SDL_RenderClear(mon->renderer);
    SDL_SetRenderScale(mon->renderer, FONT_SCALE, FONT_SCALE);

    /* Output lines */
    for (int row = 0; row < OUT_ROWS; row++) {
        float y = (float)(row * CHAR_H);
        draw_text(mon->renderer, 0, y, mon->screen[row], C_TEXT);
    }

    /* Bottom row: "-- more --" or prompt + input */
    float input_y = (float)(OUT_ROWS * CHAR_H);
    if (mon->waiting_more) {
        draw_text(mon->renderer, 0, input_y, "-- more -- (ENTER/SPACE)", C_MORE);
    } else {
        /* Prompt */
        draw_text(mon->renderer, 0, input_y, ">_", C_PROMPT);
        /* Input text */
        if (mon->input_len > 0)
            draw_text(mon->renderer, (float)(3 * CHAR_W), input_y,
                      mon->input, C_TEXT);
        /* Cursor block */
        float cx = (float)((3 + mon->input_len) * CHAR_W);
        set_color(mon->renderer, C_TEXT);
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

    mon->window = SDL_CreateWindow(
        "Memory Monitor",
        WIN_W, WIN_H,
        SDL_WINDOW_RESIZABLE);
    if (!mon->window) { free(mon); return NULL; }

    mon->renderer = SDL_CreateRenderer(mon->window, NULL);
    if (!mon->renderer) {
        SDL_DestroyWindow(mon->window);
        free(mon);
        return NULL;
    }

    SDL_HideWindow(mon->window);

    /* Seed the screen with a welcome message */
    mon_puts(mon, "CPC Memory Monitor");
    mon_puts(mon, "Commands:  D <addr> [<end_addr>]   X = exit");
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

    /* Close button on the monitor window */
    if (e->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
        e->window.windowID == SDL_GetWindowID(mon->window)) {
        mon->open = false;
        SDL_HideWindow(mon->window);
        return true;
    }

    /* Only consume key events directed at the monitor window */
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

    /* Hide window when it loses focus? No — let it stay visible. */
    return false;
}

/* screen_text — see screen_text.h */
#include "screen_text.h"
#include "cpc.h"
#include "kbd_pty.h"

#include <stdint.h>
#include <string.h>

/* ROM offset of the firmware character set. The CPC OS ROM holds 256
 * × 8-byte glyphs (codes 0x00-0xFF). For the common 7-bit ASCII range
 * we only really need 0x20-0x7F. */
#define FONT_OFFSET 0x3800
#define FONT_NCHARS 256

/* The hash is just the 8 glyph bytes packed as a u64 — fingerprinting
 * an 8x8 bitmap. Collisions across CPC font are rare enough to ignore
 * for our use case; on collision we resolve by lowest char index. */
static uint64_t s_font_hash[FONT_NCHARS];
static u8       s_font_glyph[FONT_NCHARS][8];
static bool     s_inited = false;
static bool     s_enabled = false;

void screen_text_set_enabled(bool on) { s_enabled = on; }
bool screen_text_is_enabled(void) { return s_enabled; }

/* Last emitted grid + dimensions, kept so we only push when something
 * changes (saves bandwidth on the PTY and keeps the reader's diff
 * trivial). */
#define MAX_COLS 80
#define MAX_ROWS 25
static char s_last_grid[MAX_ROWS][MAX_COLS + 1];
static int  s_last_cols = -1;
static int  s_last_rows = -1;

void screen_text_init(CPC *cpc) {
    if (s_inited) return;
    for (int c = 0; c < FONT_NCHARS; c++) {
        uint64_t h = 0;
        for (int r = 0; r < 8; r++) {
            u8 b = cpc->mem.rom_os[FONT_OFFSET + c * 8 + r];
            s_font_glyph[c][r] = b;
            h = (h << 8) | b;
        }
        s_font_hash[c] = h;
    }
    /* Force the first scan to emit a full grid. */
    memset(s_last_grid, 0, sizeof(s_last_grid));
    s_last_cols = s_last_rows = -1;
    s_inited = true;
}

/* Hamming distance over 64-bit u64s (popcount of XOR). Standard
 * trick; compiler emits popcnt on x86-64. Keep the private helper
 * distinct from NetBSD libc's popcount64(). */
static inline int screen_text_popcount64(uint64_t v) {
    return __builtin_popcountll(v);
}

/* Look up an 8-byte glyph fingerprint. First try exact match (fast
 * common case), then fall back to a nearest-neighbour search across
 * the printable range. The fallback handles CP/M+ kernel digits and
 * other glyphs that differ from the firmware font by a handful of
 * pixels — they were all '?' before. Threshold of 8 bits (out of 64)
 * keeps false matches low while still catching reasonable variants. */
static char glyph_to_char(uint64_t h) {
    if (h == 0) return ' ';     /* all-zero cell — blank */

    /* Exact match first. */
    for (int c = 0x20; c < 0x80; c++)
        if (s_font_hash[c] == h)
            return (char)c;

    /* Nearest-neighbour fallback over printable ASCII. We bias toward
     * lower-codepoint matches on ties so '0' wins over 'O' etc. */
    int best_c = 0, best_d = 9;       /* > 8-bit threshold = no match */
    for (int c = 0x20; c < 0x7F; c++) {
        int d = screen_text_popcount64(s_font_hash[c] ^ h);
        if (d < best_d) { best_d = d; best_c = c; }
    }
    if (best_d <= 8 && best_c >= 0x20) return (char)best_c;
    return '?';
}

/* Read 8 bytes from screen RAM for character cell (row, col) on mode 2
 * (1bpp, 80 cols × 25 rows, 1 byte per char column per raster). Layout:
 *   base + (raster << 11) + row*80 + col
 * for raster 0..7. Returns the 8 bytes packed as a u64 (MSB = raster 0)
 * so it's directly comparable with the font hash. */
static uint64_t cell_hash_mode2(CPC *cpc, u16 base, int row, int col) {
    uint64_t h = 0;
    int row_byte_offset = row * 80 + col;
    for (int r = 0; r < 8; r++) {
        u16 addr = (u16)(base + (r << 11) + row_byte_offset);
        h = (h << 8) | mem_read_video(&cpc->mem, addr);
    }
    return h;
}

/* Mode 1 — 2bpp, 40 cols × 25 rows. Each char column is 2 bytes wide.
 * For each raster, decode the 2 bytes back to an 8-bit "any pen != 0"
 * pattern (foreground vs border). CPC mode-1 pixel layout per byte:
 *   bit 7 = px3 high, bit 3 = px3 low
 *   bit 6 = px2 high, bit 2 = px2 low
 *   bit 5 = px1 high, bit 1 = px1 low
 *   bit 4 = px0 high, bit 0 = px0 low
 * so pen index for the four pixels in a byte is encoded across bits.
 * We don't care about pen index; we only care whether the pixel is
 * background (pen 0) or any other pen. The bitmask check is:
 *   any-pixel-on = (b & 0xF0) | (b & 0x0F) but each pixel needs both
 * its bits to be 0 for "off". A pixel is OFF iff both its bits in the
 * byte are 0; ON iff either is 1. So per-pixel on = (high_bit | low_bit).
 *
 * We then concat the per-pixel bits of the two bytes into one 8-bit
 * font row. */
static uint64_t cell_hash_mode1(CPC *cpc, u16 base, int row, int col) {
    uint64_t h = 0;
    int row_byte_offset = row * 80 + col * 2;
    for (int r = 0; r < 8; r++) {
        u16 addr = (u16)(base + (r << 11) + row_byte_offset);
        u8 b0 = mem_read_video(&cpc->mem, addr);
        u8 b1 = mem_read_video(&cpc->mem, (u16)(addr + 1));
        /* For each byte, four 2bpp pixels packed as (b7,b3)(b6,b2)(b5,b1)(b4,b0).
         * On-bit per pixel: ((b7|b3)<<3) | ((b6|b2)<<2) | ((b5|b1)<<1) | (b4|b0). */
        unsigned p0 = ((b0 >> 7) | (b0 >> 3)) & 1;
        unsigned p1 = ((b0 >> 6) | (b0 >> 2)) & 1;
        unsigned p2 = ((b0 >> 5) | (b0 >> 1)) & 1;
        unsigned p3 = ((b0 >> 4) | (b0 >> 0)) & 1;
        unsigned p4 = ((b1 >> 7) | (b1 >> 3)) & 1;
        unsigned p5 = ((b1 >> 6) | (b1 >> 2)) & 1;
        unsigned p6 = ((b1 >> 5) | (b1 >> 1)) & 1;
        unsigned p7 = ((b1 >> 4) | (b1 >> 0)) & 1;
        u8 raster_byte = (u8)((p0 << 7) | (p1 << 6) | (p2 << 5) | (p3 << 4) |
                              (p4 << 3) | (p5 << 2) | (p6 << 1) | (p7 << 0));
        h = (h << 8) | raster_byte;
    }
    return h;
}

void screen_text_tick(CPC *cpc) {
    if (!s_inited || !s_enabled) return;
    if (!kbd_pty_is_open()) return;

    /* Rate-limit to one scan every N frames to keep PTY pressure low. */
    static int countdown = 0;
    if (countdown > 0) { countdown--; return; }
    countdown = 5;     /* 50 fps / 5 = 10 scans per second */

    u8 mode = cpc->ga.screen_mode;
    int cols, rows = 25;
    uint64_t (*cell_hash)(CPC *, u16, int, int);
    switch (mode) {
    case 1:  cols = 40; cell_hash = cell_hash_mode1; break;
    case 2:  cols = 80; cell_hash = cell_hash_mode2; break;
    default: return;  /* mode 0 / 3: skip */
    }

    u16 base = (u16)((((u16)cpc->crtc.reg[12] << 8) | cpc->crtc.reg[13]) & 0x3FFF);
    /* Map CRTC's 14-bit base to the 16K-aligned screen page (bits 12-13
     * of R12 select 0x0000 / 0x4000 / 0x8000 / 0xC000). The CPC firmware
     * normally programs 0xC000. */
    u16 screen_base = (u16)((cpc->crtc.reg[12] & 0x30) << 10);

    /* Build current grid. */
    char grid[MAX_ROWS][MAX_COLS + 1];
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            uint64_t h = cell_hash(cpc, screen_base, r, c);
            char ch = glyph_to_char(h);
            grid[r][c] = ch;
        }
        grid[r][cols] = '\0';
    }
    (void)base;

    /* If unchanged from last emission, no-op. */
    if (cols == s_last_cols && rows == s_last_rows &&
        memcmp(grid, s_last_grid, (size_t)rows * (MAX_COLS + 1)) == 0)
        return;

    /* Snapshot the new grid + emit. Frame marker \f, then 25 rows
     * terminated by \r\n so consumers can split on \f and \n cleanly. */
    memcpy(s_last_grid, grid, sizeof(s_last_grid));
    s_last_cols = cols;
    s_last_rows = rows;

    /* Stream the whole frame as a single write() — emitting a thousand
     * bytes per frame via per-byte writes saturates the PTY buffer and
     * blocks the kbd-input drain. */
    char out[1 + MAX_ROWS * (MAX_COLS + 2)];
    int pos = 0;
    out[pos++] = '\f';
    for (int r = 0; r < rows; r++) {
        memcpy(out + pos, grid[r], (size_t)cols);
        pos += cols;
        out[pos++] = '\r';
        out[pos++] = '\n';
    }
    kbd_pty_emit_buf(out, pos);
}

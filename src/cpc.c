#include "cpc.h"
#include <stdio.h>
#include <string.h>

/* ---- Z80 bus callbacks ---- */

static u8 bus_mem_read(void *ctx, u16 addr) {
    CPC *cpc = ctx;
    return mem_read(&cpc->mem, addr);
}
static void bus_mem_write(void *ctx, u16 addr, u8 val) {
    CPC *cpc = ctx;
    mem_write(&cpc->mem, addr, val);
}

static u8 bus_io_read(void *ctx, u16 port) {
    CPC *cpc = ctx;
    u8 hi = port >> 8;

    /* CRTC read: A14=0 (hi & ~0x40 → 0xBF area) */
    if (!(hi & 0x40)) {
        u8 func = (port >> 8) & 0x03;
        if (func == 0x03) return crtc_read(&cpc->crtc);
        return 0xFF;
    }
    /* PPI: A11=0 selects PPI (0xF4xx/0xF5xx/0xF6xx/0xF7xx) */
    if (!(hi & 0x08)) {
        return ppi_read(&cpc->ppi, (port >> 8) & 0x03);
    }
    return 0xFF;
}

static void bus_io_write(void *ctx, u16 port, u8 val) {
    CPC *cpc = ctx;
    u8 hi = port >> 8;

    /* Gate Array: A15=0, A14=1 → 0x7Fxx */
    if (!(hi & 0x80) && (hi & 0x40)) {
        ga_write(&cpc->ga, val);
        cpc->mem.lower_rom_enabled = cpc->ga.lower_rom;
        cpc->mem.upper_rom_enabled = cpc->ga.upper_rom;
        /* 6128 RAM banking */
        if ((val & 0xC0) == 0xC0 && cpc->model == MODEL_6128)
            cpc->mem.ram_bank = val & 0x07;
        return;
    }
    /* CRTC: A14=0 → 0xBCxx (select, A8=0) / 0xBDxx (write, A8=1) */
    if (!(hi & 0x40)) {
        if (!(hi & 0x01)) crtc_select(&cpc->crtc, val);
        else               crtc_write(&cpc->crtc, val);
        return;
    }
    /* PPI: A11=0 → 0xF4 (port A), 0xF5 (B), 0xF6 (C), 0xF7 (ctrl) */
    if (!(hi & 0x08)) {
        ppi_write(&cpc->ppi, hi & 0x03, val);
        /* Route PSG control bits from PPI port C */
        u8 psg_ctrl = (cpc->ppi.port_c >> 6) & 0x03;
        if (psg_ctrl == 0x03) psg_select(&cpc->psg, cpc->ppi.port_a);
        else if (psg_ctrl == 0x02) psg_write(&cpc->psg, cpc->ppi.port_a);
        else if (psg_ctrl == 0x01) {
            psg_set_kbd_row(&cpc->psg, kbd_read_row(&cpc->kbd, cpc->ppi.kbd_row));
            cpc->ppi.port_a = psg_read(&cpc->psg);
        }
        return;
    }
}

/* ---- Init / destroy ---- */

int cpc_init(CPC *cpc, CpcModel model, const char *rom_os, const char *rom_basic) {
    memset(cpc, 0, sizeof(*cpc));
    cpc->model = model;
    cpc->cpu_clk_hz = 4000000;
    /* 50 Hz PAL: 4 MHz / 50 = 80 000 cycles per frame */
    cpc->cycles_per_frame = cpc->cpu_clk_hz / 50;

    mem_init(&cpc->mem);
    if (mem_load_rom(&cpc->mem, rom_os, rom_basic) < 0)
        return -1;

    z80_init(&cpc->cpu);
    z80_reset(&cpc->cpu);

    ga_init(&cpc->ga);
    crtc_init(&cpc->crtc);
    ppi_init(&cpc->ppi);
    psg_init(&cpc->psg);
    kbd_init(&cpc->kbd);

    cpc->bus.mem_read  = bus_mem_read;
    cpc->bus.mem_write = bus_mem_write;
    cpc->bus.io_read   = bus_io_read;
    cpc->bus.io_write  = bus_io_write;
    cpc->bus.ctx       = cpc;

    const char *title = (model == MODEL_464) ? "CPC 464" : "CPC 6128";
    if (display_init(&cpc->display, title) < 0)
        return -1;

    return 0;
}

void cpc_destroy(CPC *cpc) {
    display_destroy(&cpc->display);
}

/* ---- Pixel rendering ----
 *
 * Each CRTC character clock fetches 2 bytes and outputs 16 pixels.
 * Mode 2: 8 pixels/byte, 1 bit per pixel  → 1:1 mapping (16 px)
 * Mode 1: 4 pixels/byte, 2 bits per pixel → each pixel doubled  (16 px)
 * Mode 0: 2 pixels/byte, 4 bits per pixel → each pixel ×4       (16 px)
 *
 * Video address: A[15:14] = MA[13:12], A[13:11] = RA[2:0], A[10:1] = MA[9:0]
 */

static inline u32 ink_rgb(GateArray *ga, u8 pen) {
    return ga_hw_palette[ga->ink[pen < GA_NUM_INKS ? pen : 0]];
}

static void render_char(u32 *row, int px, GateArray *ga, u8 b0, u8 b1) {
    switch (ga->screen_mode) {

    case 2: /* 1 bpp, 8 pixels/byte, 1:1 → 16 px */
        for (int b = 0; b < 2; b++) {
            u8 byte = b ? b1 : b0;
            for (int p = 0; p < 8; p++) {
                int x = px + b * 8 + p;
                if ((unsigned)x < CPC_SCREEN_W)
                    row[x] = ink_rgb(ga, (byte >> (7 - p)) & 1);
            }
        }
        break;

    case 1: /* 2 bpp, 4 pixels/byte, doubled → 16 px */
        for (int b = 0; b < 2; b++) {
            u8 byte = b ? b1 : b0;
            for (int p = 0; p < 4; p++) {
                u8 pen = (u8)(((byte >> (3 - p)) & 1) << 1 | ((byte >> (7 - p)) & 1));
                u32 c  = ink_rgb(ga, pen);
                int x  = px + b * 8 + p * 2;
                if ((unsigned)x     < CPC_SCREEN_W) row[x]     = c;
                if ((unsigned)(x+1) < CPC_SCREEN_W) row[x + 1] = c;
            }
        }
        break;

    case 0: /* 4 bpp, 2 pixels/byte, ×4 → 16 px */
        for (int b = 0; b < 2; b++) {
            u8 byte = b ? b1 : b0;
            u8 pen0 = (u8)(((byte>>7)&1)<<3 | ((byte>>5)&1)<<2 | ((byte>>3)&1)<<1 | ((byte>>1)&1));
            u8 pen1 = (u8)(((byte>>6)&1)<<3 | ((byte>>4)&1)<<2 | ((byte>>2)&1)<<1 | ((byte>>0)&1));
            u32 c0  = ink_rgb(ga, pen0);
            u32 c1  = ink_rgb(ga, pen1);
            for (int i = 0; i < 4; i++) {
                int x = px + b * 8 + i;
                if ((unsigned)x < CPC_SCREEN_W) row[x] = c0;
                x = px + b * 8 + 4 + i;
                if ((unsigned)x < CPC_SCREEN_W) row[x] = c1;
            }
        }
        break;
    }
}

/* ---- Frame execution ----
 *
 * Raster grid: raster_x in char clocks (0 = first char after hsync end).
 * Default CRTC timing (H total=63, H sync pos=46, H sync width=14):
 *   chars 60-63 = 4 left border chars   → raster_x 0-3   → px 0-63
 *   chars  0-39 = 40 displayed chars    → raster_x 4-43  → px 64-703
 *   chars 40-47 = right border/overflow → raster_x 44-51 → px 704-767+
 *
 * For Y: vsync fires at char row 30 (scan 240). After vsync (16 lines)
 * and top border (48 lines = 6 rows), display starts at scan 304 mod frame.
 * We set raster_y = -64 at vsync so raster_y=0 when display starts.
 */

/* Left border offset in char clocks = H_total+1 - H_sync_pos - H_sync_width */
#define RASTER_X_AFTER_HSYNC  -1
/* Scan lines from vsync rising edge to first displayed line:
 * 16 (vsync pulse) + 56 (7 top border char rows × 8) = 72 */
#define VSYNC_TO_DISPLAY_LINES 40

void cpc_frame(CPC *cpc) {
    int target = cpc->cycles_per_frame + cpc->cycle_debt;
    int done   = 0;

    while (done < target) {
        /* Capture CRTC state BEFORE tick for this character clock */
        u16  cur_ma = cpc->crtc.ma;
        u8   cur_ra = cpc->crtc.vlc;
        bool cur_de = cpc->crtc.display_enable;

        int t = z80_step(&cpc->cpu, &cpc->bus);
        done += t;

        /* CRTC ticks at 1 MHz = every 4 CPU cycles */
        for (int i = 0; i < t; i += 4) {
            crtc_tick(&cpc->crtc);

            bool new_hsync = crtc_hsync(&cpc->crtc);
            bool new_vsync = crtc_vsync(&cpc->crtc);

            /* GA interrupt counter on hsync rising edge */
            if (new_hsync && !cpc->prev_hsync)
                ga_hsync(&cpc->ga);

            ppi_set_vsync(&cpc->ppi, new_vsync);

            /* --- Edge detection BEFORE render so raster resets apply this tick --- */

            /* HSYNC falling edge → start of new scan line */
            if (!new_hsync && cpc->prev_hsync) {
                cpc->raster_x = RASTER_X_AFTER_HSYNC;
                cpc->raster_y++;
            }

            /* VSYNC rising edge → start of new frame */
            if (new_vsync && !cpc->prev_vsync) {
                cpc->raster_y = -VSYNC_TO_DISPLAY_LINES;
                display_present(&cpc->display);
            }

            cpc->prev_hsync = new_hsync;
            cpc->prev_vsync = new_vsync;

            /* --- Render 16 pixels for this char clock --- */
            int px = cpc->raster_x * 16;
            int py = cpc->raster_y;

            if (py >= 0 && py < CPC_SCREEN_H && px >= 0 && px < CPC_SCREEN_W) {
                u32 *row = cpc->display.pixels + py * CPC_SCREEN_W;
                if (cur_de) {
                    /* Video address: bank=MA[13:12], raster=RA[2:0], col=MA[9:0] */
                    u16 bank = (cur_ma >> 12) & 3;
                    u16 col  = cur_ma & 0x3FF;
                    u16 addr = (u16)((bank << 14) | ((cur_ra & 7) << 11) | (col << 1));
                    u8  b0   = mem_read_video(&cpc->mem, addr);
                    u8  b1   = mem_read_video(&cpc->mem, (u16)(addr + 1));
                    render_char(row, px, &cpc->ga, b0, b1);
                } else {
                    u32 c = ga_hw_palette[cpc->ga.ink[16]]; /* border */
                    for (int p = 0; p < 16 && px + p < CPC_SCREEN_W; p++)
                        row[px + p] = c;
                }
            }

            cpc->raster_x++;

            /* Capture next char's pre-tick state */
            cur_ma = cpc->crtc.ma;
            cur_ra = cpc->crtc.vlc;
            cur_de = cpc->crtc.display_enable;
        }

        /* Deliver pending Gate Array interrupt */
        if (cpc->ga.interrupt_pending) {
            cpc->ga.interrupt_pending = false;
            z80_interrupt(&cpc->cpu);
        }
    }

    cpc->cycle_debt = done - target;
}

void cpc_key_event(CPC *cpc, SDL_Scancode sc, bool pressed) {
    kbd_sdl_key(&cpc->kbd, sc, pressed);
}

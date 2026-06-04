#include "cpc.h"
#include "net4cpc.h"
#include <string.h>
#include <stdio.h>

/* Set to 1 at runtime to trace CRTC/GA writes to stderr */
int cpc_trace_io = 0;
int cpc_trace_palette = 0;
int cpc_frame_count = 0;
int cpc_trace_input = 0;

#define AUDIO_SAMPLE_RATE   44100
#define AUDIO_SAMPLES_FRAME (AUDIO_SAMPLE_RATE / 50)   /* 882 samples @ 50 Hz */
#define PSG_CLOCK_HZ        1000000                    /* CPC PSG: 4 MHz / 4 */

/* ---- Z80 bus callbacks ---- */

static u8 bus_mem_read(void *ctx, u16 addr) {
    CPC *cpc = ctx;
    /* M4 board maps its own response/config buffers on the expansion bus when
     * M4ROM is the active upper ROM. Reads return from the M4 board's RAM,
     * not CPC RAM — this is critical because CPC screen memory lives at
     * 0xC000-0xFFFF and we'd otherwise corrupt the display.
     *   0xE800-0xF3FF → m4_card.bus_mem (rom_response, 0xC00 bytes — large
     *                   enough for 2KB C_READ payload starting at resp+4)
     *   0xF400-0xF4FF → m4_card.cfg_mem (rom_config: jump_vec, init_count)
     *   0xFE00-0xFE4F → m4_card.sock_mem (sock_info: 5 × 16 bytes for NETAPI) */
    /* Bus bypass: triggers either with M4ROM paged in (slot 6) or while
     * the M4 board is in "RAM mode" (set only by network-class commands —
     * see m4_ackport_write). ram_mode auto-clears via a small frame timer
     * driven from m4_tick.
     *
     * Under ram_mode we also redirect upper-ROM code fetches at
     * 0xC000-0xE7FF to M4ROM bytes regardless of which slot the CPU has
     * selected — that's how real hardware lets the SymbOS daemon
     * "out (0xDF00), 0" then "JP <m4 helper>" and still reach the helper
     * code that lives in M4ROM. */
    bool bypass_slot = cpc->mem.upper_rom_enabled
                       && cpc->mem.upper_rom_select == M4_ROM_SLOT;
    if (cpc->m4 && (bypass_slot || cpc->m4_card.ram_mode)) {
        u8 v = 0; bool hit = true;
        /* Skip the bypass for likely stack POP/RET reads (addr matches SP
         * or SP+1). The SymbOS netd-m4c.exe daemon places its stack at
         * ~0xF076 — inside our bus_mem mapping — and without this guard
         * RET would pop bus_mem bytes (zeros / response garbage) instead
         * of the real return address, derailing m4cred/m4crcv. */
        bool is_stack_pop = (addr == cpc->cpu.sp) || (addr == (u16)(cpc->cpu.sp + 1));
        if (is_stack_pop) {
            hit = false;
        }
        else if (addr >= 0xE800 && addr < 0xF400) {
            /* Each read in the response area decrements the current
             * command's expected-byte counter. When it hits zero we know
             * the caller has consumed its response, so if a pending
             * C_NETRSSI snapshot exists (because that prior RSSI ACK was
             * clobbered before the netd daemon could read it), restore
             * bus_mem[3..4] now so the daemon's later read still finds
             * the RSSI bytes. */
            if (addr >= 0xE803 && cpc->m4_card.last_resp_len > 0)
                cpc->m4_card.last_resp_len--;
            if (cpc->m4_card.last_resp_len == 0 && cpc->m4_card.rssi_resp_pending) {
                cpc->m4_card.bus_mem[3] = cpc->m4_card.rssi_resp_save[0];
                cpc->m4_card.bus_mem[4] = cpc->m4_card.rssi_resp_save[1];
                cpc->m4_card.rssi_resp_pending = false;
            }
            v = cpc->m4_card.bus_mem[addr - 0xE800];
        }
        else if (addr >= 0xF400 && addr < 0xF500)
            v = cpc->m4_card.cfg_mem[addr - 0xF400];
        else if (addr >= 0xFE00 && addr < 0xFF00)
            v = cpc->m4_card.sock_mem[addr - 0xFE00];
        else
            hit = false;
        if (hit) {
            if (!bypass_slot && cpc->m4_card.ram_mode
                    && --cpc->m4_card.ram_mode_reads <= 0)
                cpc->m4_card.ram_mode = false;
            return v;
        }
    }
    return mem_read(&cpc->mem, addr);
}
static void bus_mem_write(void *ctx, u16 addr, u8 val) {
    CPC *cpc = ctx;
    mem_write(&cpc->mem, addr, val);
    /* When palette tracing, log all writes to the firmware workspace area
     * (0xB700-0xB7FF) so we can find the real dirty-flag address on each model. */
    /* When tracing, log writes to suspected 464 palette buffer B1D9-B1FC. */
    if (cpc_trace_palette && cpc_frame_count > 520
            && addr >= 0xB1D9 && addr <= 0xB1FC)
        fprintf(stderr, "[f%04d memw] %04X <- %02X  lrom=%d\n",
                cpc_frame_count, addr, val, cpc->mem.lower_rom_enabled);
}

static u8 bus_io_read(void *ctx, u16 port) {
    CPC *cpc = ctx;
    u8 hi = port >> 8;
    u8 result;

    /* CRTC read: A14=0 (hi & ~0x40 → 0xBF area) */
    if (!(hi & 0x40)) {
        u8 func = (port >> 8) & 0x03;
        if (func == 0x03)      result = crtc_read(&cpc->crtc);
        else if (func == 0x02) result = crtc_read_status(&cpc->crtc);
        else                   result = 0xFF;
    }
    /* PPI: A11=0 selects PPI (0xF4xx/0xF5xx/0xF6xx/0xF7xx) */
    else if (!(hi & 0x08)) {
        result = ppi_read(&cpc->ppi, (port >> 8) & 0x03);
    }
    /* FDC: hi=0xFB → status (lo bit 0=0) or data (lo bit 0=1) */
    else if (hi == 0xFB) {
        u8 lo = port & 0xFF;
        result = (lo & 0x01) ? fdc_read_data(&cpc->fdc)
                             : fdc_read_status(&cpc->fdc);
    }
    else if (hi == 0xFA) {
        result = 0xFF;
    }
    /* Albireo CH376: hi=0xFE, lo=0x80/0x81 (claim before M4 wide decode) */
    else if (cpc->mx4 && cpc->albireo && hi == 0xFE && (port & 0xFE) == 0x80) {
        result = ch376_read(&cpc->ch376, (u8)(port & 0x01));
    }
    /* M4 DATAPORT: hi=0xFE or 0xFF (read = ready/status) */
    else if (cpc->mx4 && cpc->m4 && (hi == 0xFE || hi == 0xFF)) {
        result = m4_dataport_read(&cpc->m4_card);
    }
    /* hi=0xFD: Mouse (0xFD10), IDE (0xFD06,0xFD08-0xFD0F), RTC (0xFD14), Net4CPC (0xFD20-0xFD23) */
    else if (cpc->mx4 && hi == 0xFD) {
        u8 lo = port & 0xFF;
        if (cpc->symbiface_mouse && lo == 0x10)
            result = mouse_read(&cpc->mouse);
        else if (cpc->symbiface_ide && (lo == 0x06 || (lo >= 0x08 && lo <= 0x0F)))
            result = ide_read(&cpc->ide_chip, lo);
        else if (cpc->rtc && lo == 0x14)
            result = rtc_read_data(&cpc->rtc_chip);
        else if (cpc->net4cpc && lo >= 0x20 && lo <= 0x23)
            result = net4cpc_in(lo & 0x03);
        else if (cpc->symbnet && (lo == 0x30 || lo == 0x31))
            result = symbnet_port_read(&cpc->symbnet_card, lo);
        else
            result = 0xFF;
    }
    else {
        result = 0xFF;
    }

    return result;
}

static void bus_io_write(void *ctx, u16 port, u8 val) {
    CPC *cpc = ctx;
    u8 hi = port >> 8;

    /* Gate Array: A15=0, A14=1 → 0x7Fxx */
    if (!(hi & 0x80) && (hi & 0x40)) {
        if (cpc_trace_palette && cpc_frame_count > 520 && (val & 0xC0) == 0x40)
            fprintf(stderr, "[f%04d ga] pen=%02X col=%02X\n",
                    cpc_frame_count, cpc->ga.selected_pen, val & 0x1F);
        else if (cpc_trace_palette && cpc_frame_count > 520
                 && (val & 0xC0) == 0x00 && val <= 0x10)
            fprintf(stderr, "[f%04d ga] select pen=%02X\n",
                    cpc_frame_count, val);
        ga_write(&cpc->ga, val);
        cpc->mem.lower_rom_enabled = cpc->ga.lower_rom;
        cpc->mem.upper_rom_enabled = cpc->ga.upper_rom;
        /* RAM banking — bits[5:0] of data select bank group and mode.
         * Standard on 6128; emulator extension enables it on 464 too
         * when memory > 64 KB is configured.
         * Yarek extension: port address bits A10-A8 carry an upper bank_high
         * selector for RAM above 576 KB. Port 0x7Fxx = bank_high 0 (DK'tronics
         * compatible); 0x7Exx = 1, 0x7Dxx = 2, 0x7Cxx = 3. bank_high is packed
         * into ram_bank bits[7:6] so banked_ram_offset() can read it. */
        if ((val & 0xC0) == 0xC0) {
            u8 bank_high = ((hi & 0xFC) == 0x7C) ? ((~hi) & 0x03) : 0;
            cpc->mem.ram_bank = (u8)((bank_high << 6) | (val & 0x3F));
        }
        return;
    }
    /* CRTC: A14=0 → 0xBCxx (select, A8=0) / 0xBDxx (write, A8=1) */
    if (!(hi & 0x40)) {
        if (!(hi & 0x01)) {
            crtc_select(&cpc->crtc, val);
        } else {
            if (cpc_trace_io)
                fprintf(stderr, "CRTC R%-2d = %3d (0x%02X)\n",
                        cpc->crtc.selected, val, val);
            crtc_write(&cpc->crtc, val);
        }
        return;
    }
    /* Upper ROM select: A15=1, A14=1, A13=0 → 0xC0xx–0xDFxx */
    if ((hi & 0xE0) == 0xC0) {
        cpc->mem.upper_rom_select = val;
        return;
    }
    /* PPI: A11=0 → 0xF4 (port A), 0xF5 (B), 0xF6 (C), 0xF7 (ctrl) */
    if (!(hi & 0x08)) {
        ppi_write(&cpc->ppi, hi & 0x03, val);
        /* Cassette motor follows PPI port C bit 4. */
        tape_set_motor(&cpc->tape, (cpc->ppi.port_c & 0x10) != 0);
        /* Route PSG control bits from PPI port C */
        u8 psg_ctrl = (cpc->ppi.port_c >> 6) & 0x03;
        if (psg_ctrl == 0x03) psg_select(&cpc->psg, cpc->ppi.port_a);
        else if (psg_ctrl == 0x02) psg_write(&cpc->psg, cpc->ppi.port_a);
        else if (psg_ctrl == 0x01) {
            psg_set_kbd_row(&cpc->psg, kbd_read_row(&cpc->kbd, cpc->ppi.kbd_row));
            /* PSG read mode on CPC always reads I/O port A (reg 14 = keyboard matrix).
             * Bypass psg_read() to avoid depending on psg->selected being 14. */
            cpc->ppi.port_a = cpc->psg.kbd_data;
            if (cpc_trace_input && cpc->ppi.kbd_row == 9)
                fprintf(stderr, "[input] kbd scan row9 = %02X  (matrix=%02X)\n",
                        cpc->ppi.port_a, cpc->kbd.matrix[9]);
        }
        return;
    }
    /* Albireo CH376: hi=0xFE, lo=0x80/0x81 (claim before M4 wide decode) */
    if (cpc->mx4 && cpc->albireo && hi == 0xFE && (port & 0xFE) == 0x80) {
        ch376_write(&cpc->ch376, (u8)(port & 0x01), val);
        return;
    }
    /* M4 DATAPORT: hi=0xFE or 0xFF — accumulate command byte */
    if (cpc->mx4 && cpc->m4 && (hi == 0xFE || hi == 0xFF)) {
        m4_dataport_write(&cpc->m4_card, val); return;
    }
    /* M4 ACKPORT: hi=0xFC — trigger command execution */
    if (cpc->mx4 && cpc->m4 && hi == 0xFC) {
        if (m4_ackport_write(&cpc->m4_card, &cpc->mem))
            z80_nmi(&cpc->cpu);
        return;
    }
    /* FDC motor: hi=0xFA, write */
    if (hi == 0xFA) { fdc_motor_write(&cpc->fdc, val); return; }
    /* FDC data: hi=0xFB, lo bit 7 = 0, write */
    if (hi == 0xFB) {
        u8 lo = port & 0xFF;
        if (!(lo & 0x80)) fdc_write_data(&cpc->fdc, val);
        return;
    }
    /* hi=0xFD: IDE (0xFD06, 0xFD08-0xFD0F), RTC (0xFD14/0xFD15), Net4CPC (0xFD20-0xFD23) */
    if (cpc->mx4 && hi == 0xFD) {
        u8 lo = port & 0xFF;
        if (cpc->symbiface_ide && (lo == 0x06 || (lo >= 0x08 && lo <= 0x0F))) {
            ide_write(&cpc->ide_chip, lo, val); return;
        }
        if (cpc->rtc) {
            if (lo == 0x15) { rtc_write_addr(&cpc->rtc_chip, val); return; }
            if (lo == 0x14) { rtc_write_data(&cpc->rtc_chip, val); return; }
        }
        if (cpc->net4cpc && lo >= 0x20 && lo <= 0x23)
            net4cpc_out(lo & 0x03, val);
        if (cpc->symbnet && (lo == 0x30 || lo == 0x31))
            symbnet_port_write(&cpc->symbnet_card, lo, val);
        /* "1984 compatibility shim" helper traps. The patched M4ROM
         * helper table points the SymbOS daemon's m4crcv into bus_mem;
         * the stub there is "LD BC, 0xFD3F ; OUT (C), A". When we see
         * that OUT, we run the equivalent bank-aware bulk copy in C and
         * jump PC to IX (the return address the daemon passed). */
        if (cpc->m4 && (lo == 0x3E || lo == 0x3F)) {
            u16 src    = cpc->cpu.hl;
            u16 dest   = cpc->cpu.de;
            u16 length = ((cpc->cpu.iy >> 8) << 8) | cpc->cpu.c;
            u8  dest_bank   = cpc->cpu.a;
            u8  source_bank = (u8)(cpc->cpu.iy & 0xFF);
            u16 retaddr     = cpc->cpu.ix;
            u8  saved_bank  = cpc->mem.ram_bank;

            if ((dest_bank & 0xC0) == 0xC0)
                cpc->mem.ram_bank = dest_bank & 0x3F;

            for (u16 i = 0; i < length; i++) {
                u8 b;
                u16 sa = (u16)(src + i);
                u16 da = (u16)(dest + i);
                if (lo == 0x3F) {
                    /* hreceive: M4 buffer → application memory */
                    if (sa >= 0xE800 && sa < 0xF400)
                        b = cpc->m4_card.bus_mem[sa - 0xE800];
                    else
                        b = mem_read(&cpc->mem, sa);
                    mem_write(&cpc->mem, da, b);
                } else {
                    /* hsend: application memory → M4 buffer */
                    b = mem_read(&cpc->mem, sa);
                    if (da >= 0xE800 && da < 0xF400)
                        cpc->m4_card.bus_mem[da - 0xE800] = b;
                    else
                        mem_write(&cpc->mem, da, b);
                }
            }

            if ((source_bank & 0xC0) == 0xC0)
                cpc->mem.ram_bank = source_bank & 0x3F;
            else
                cpc->mem.ram_bank = saved_bank;

            cpc->cpu.pc = cpc->cpu.ix;
            (void)retaddr;
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
    disk_init(&cpc->drive[0]);
    disk_init(&cpc->drive[1]);
    fdc_init(&cpc->fdc, &cpc->drive[0], &cpc->drive[1]);
    rtc_init(&cpc->rtc_chip);
    ide_init(&cpc->ide_chip);
    mouse_init(&cpc->mouse);
    m4_init(&cpc->m4_card, "");
    symbnet_init(&cpc->symbnet_card, &cpc->m4_card);
    ch376_init(&cpc->ch376);
    tape_init(&cpc->tape);
    net4cpc_reset();

    cpc->bus.mem_read  = bus_mem_read;
    cpc->bus.mem_write = bus_mem_write;
    cpc->bus.io_read   = bus_io_read;
    cpc->bus.io_write  = bus_io_write;
    cpc->bus.ctx       = cpc;

    const char *title = (model == MODEL_464)
        ? "CPC 464  |  F4 = screenshot   F5 = reset   F8 = monitor   F9 = options   F11 = fullscreen"
        : "CPC 6128  |  F4 = screenshot   F5 = reset   F8 = monitor   F9 = options   F11 = fullscreen";
    if (display_init(&cpc->display, title) < 0)
        return -1;

    SDL_AudioSpec spec = { SDL_AUDIO_S16, 1, AUDIO_SAMPLE_RATE };
    cpc->audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (cpc->audio_stream)
        SDL_ResumeAudioStreamDevice(cpc->audio_stream);

    return 0;
}

void cpc_reset(CPC *cpc) {
    z80_reset(&cpc->cpu);
    ga_init(&cpc->ga);
    crtc_init(&cpc->crtc);
    ppi_init(&cpc->ppi);
    psg_init(&cpc->psg);
    kbd_init(&cpc->kbd);
    fdc_reset(&cpc->fdc);
    rtc_init(&cpc->rtc_chip);
    ide_reset(&cpc->ide_chip);  /* keeps image file open across warm reset */
    mouse_init(&cpc->mouse);    /* clear accumulated deltas; capture state managed by main */
    m4_reset(&cpc->m4_card);
    ch376_reset(&cpc->ch376);
    cpc->mem.lower_rom_enabled = true;
    cpc->mem.upper_rom_enabled = true;
    cpc->mem.ram_bank = 0;
    cpc->raster_x  = 0;
    cpc->raster_y  = 0;
    cpc->prev_hsync = false;
    cpc->prev_vsync = false;
    cpc->cycle_debt = 0;
}

void cpc_destroy(CPC *cpc) {
    if (cpc->audio_stream) SDL_DestroyAudioStream(cpc->audio_stream);
    disk_eject(&cpc->drive[0]);
    disk_eject(&cpc->drive[1]);
    ide_close(&cpc->ide_chip);
    ch376_close(&cpc->ch376);
    tape_eject(&cpc->tape);
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
            u8 pen0 = (u8)(((byte>>1)&1)<<3 | ((byte>>5)&1)<<2 | ((byte>>3)&1)<<1 | ((byte>>7)&1));
            u8 pen1 = (u8)(((byte>>0)&1)<<3 | ((byte>>4)&1)<<2 | ((byte>>2)&1)<<1 | ((byte>>6)&1));
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
    if (cpc->paused && !cpc->step_once) return;
    bool was_stepping = cpc->step_once;
    cpc->step_once = false;

    int target = cpc->cycles_per_frame + cpc->cycle_debt;
    int done   = 0;
    bool stop_early = false;

    while (done < target) {
        /* Capture CRTC state BEFORE tick for this character clock */
        u16  cur_ma = cpc->crtc.ma;
        u8   cur_ra = cpc->crtc.vlc;
        bool cur_de = cpc->crtc.display_enable;

        int t = z80_step(&cpc->cpu, &cpc->bus);
        done += t;
        /* Advance the cassette by this instruction's T-states and push
         * the resulting level into the PPI's Port B bit 7 mirror. */
        tape_step(&cpc->tape, t);
        ppi_set_tape_level(&cpc->ppi, tape_level(&cpc->tape));
        /* Sample the cassette signal at audio rate (~90.7 cycles/sample
         * for 4 MHz / 44.1 kHz) so it can be mixed into PSG output. */
        cpc->tape_audio_cycles += t * AUDIO_SAMPLE_RATE;
        while (cpc->tape_audio_cycles >= cpc->cpu_clk_hz &&
               cpc->tape_audio_pos < AUDIO_SAMPLES_FRAME) {
            cpc->tape_audio[cpc->tape_audio_pos++] =
                (tape_level(&cpc->tape) & 0x80) ? 2500 : -2500;
            cpc->tape_audio_cycles -= cpc->cpu_clk_hz;
        }

        /* CRTC ticks at 1 MHz = every 4 CPU cycles. Track leftover cycles so we
         * don't over-tick across instructions whose cycle count isn't a multiple
         * of 4 (Caprice32 uses CPC-rounded cycles per opcode; we use raw Z80
         * cycle counts and accumulate the remainder here). */
        cpc->crtc_cycle_acc += t;
        while (cpc->crtc_cycle_acc >= 4) {
            cpc->crtc_cycle_acc -= 4;
            crtc_tick(&cpc->crtc);

            bool new_hsync = crtc_hsync(&cpc->crtc);
            bool new_vsync = crtc_vsync(&cpc->crtc);

            /* GA interrupt counter on hsync falling edge (matches Caprice32) */
            if (!new_hsync && cpc->prev_hsync)
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
                ga_vsync_start(&cpc->ga);
                display_upload(&cpc->display);
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
            ga_irq_ack(&cpc->ga);
            z80_interrupt(&cpc->cpu);
        }

        /* Breakpoint check */
        for (int b = 0; b < CPC_MAX_BREAKPOINTS; b++) {
            if (cpc->bp_enabled[b] && cpc->cpu.pc == cpc->breakpoints[b]) {
                cpc->paused   = true;
                stop_early    = true;
                break;
            }
        }
        if (stop_early || was_stepping) break;
    }

    cpc->cycle_debt = stop_early ? 0 : (done - target);
    cpc_frame_count++;

    /* Drive M4 sockets so non-blocking TCP work makes progress while CPC
     * code polls sock_info between commands. */
    if (cpc->m4) m4_tick(&cpc->m4_card);
    if (cpc->symbnet) symbnet_tick(&cpc->symbnet_card);

    /* Fallback palette flush — CPC 6128.
     * The 6128 firmware ink routine writes 0xFF to 0xB7F7 and stores the new
     * palette in 0xB7D4-0xB7E4.  Some games deactivate the firmware flush task
     * (e.g. Spindizzy), leaving 0xB7F7=0xFF and the palette unflushed.
     * We detect this by checking 0xB7F7=0xFF AND all 17 palette buffer bytes
     * are valid hardware colour indices (0x00-0x1F).  Values outside that range
     * (e.g. 0xFF or 0x55 written by a diagnostic RAM fill) suppress the flush,
     * preventing false triggers during memory tests. */
    if (mem_read(&cpc->mem, 0xB7F7) == 0xFF) {
        bool palette_valid = true;
        for (int p = 0; p < 17; p++) {
            if (mem_read(&cpc->mem, (u16)(0xB7D4 + p)) > 0x1F) {
                palette_valid = false;
                break;
            }
        }
        if (cpc_trace_palette) {
            fprintf(stderr, "[palette] B7F7=FF  valid=%d  buf:",
                    palette_valid);
            for (int p = 0; p < 17; p++)
                fprintf(stderr, " %02X",
                        mem_read(&cpc->mem, (u16)(0xB7D4 + p)));
            fprintf(stderr, "\n");
        }
        if (palette_valid) {
            for (int p = 0; p < 17; p++) {
                u8 hw  = mem_read(&cpc->mem, (u16)(0xB7D4 + p));
                u8 pen = (p == 0) ? 0x10 : (u8)(p - 1);
                ga_write(&cpc->ga, pen);
                ga_write(&cpc->ga, (u8)(0x40 | (hw & 0x1F)));
            }
            if (cpc_trace_palette)
                fprintf(stderr, "[palette] flushed → B7F7 cleared\n");
            mem_write(&cpc->mem, 0xB7F7, 0x00);
        }
    }

    /* Fallback palette flush — CPC 464.
     * The 464 firmware uses 0xB1FC as dirty flag and 0xB1D9-0xB1E9 as the
     * palette buffer (17 bytes: border first, then inks 0-15).  Same validity
     * guard as above: only flush when all bytes are in 0x00-0x1F range. */
    if (mem_read(&cpc->mem, 0xB1FC) == 0xFF) {
        bool palette_valid = true;
        for (int p = 0; p < 17; p++) {
            if (mem_read(&cpc->mem, (u16)(0xB1D9 + p)) > 0x1F) {
                palette_valid = false;
                break;
            }
        }
        if (cpc_trace_palette) {
            fprintf(stderr, "[palette464] B1FC=FF  valid=%d  buf:",
                    palette_valid);
            for (int p = 0; p < 17; p++)
                fprintf(stderr, " %02X",
                        mem_read(&cpc->mem, (u16)(0xB1D9 + p)));
            fprintf(stderr, "\n");
        }
        if (palette_valid) {
            for (int p = 0; p < 17; p++) {
                u8 hw  = mem_read(&cpc->mem, (u16)(0xB1D9 + p));
                u8 pen = (p == 0) ? 0x10 : (u8)(p - 1);
                ga_write(&cpc->ga, pen);
                ga_write(&cpc->ga, (u8)(0x40 | (hw & 0x1F)));
            }
            if (cpc_trace_palette)
                fprintf(stderr, "[palette464] flushed → B1FC cleared\n");
            mem_write(&cpc->mem, 0xB1FC, 0x00);
        }
    }

    /* Push one frame of PSG audio to SDL (skip on breakpoint/step to avoid burst) */
    if (!stop_early && cpc->audio_stream) {
        s16 audio_buf[AUDIO_SAMPLES_FRAME];
        psg_render(&cpc->psg, audio_buf, AUDIO_SAMPLES_FRAME,
                   PSG_CLOCK_HZ, AUDIO_SAMPLE_RATE);
        /* Mix in the cassette signal — captured per-sample inside the Z80
         * step loop above. Saturating add so we don't wrap on s16 overflow. */
        if (cpc->tape.present && cpc->tape.motor) {
            for (int i = 0; i < cpc->tape_audio_pos; i++) {
                int v = (int)audio_buf[i] + (int)cpc->tape_audio[i];
                if (v >  32767) v =  32767;
                if (v < -32768) v = -32768;
                audio_buf[i] = (s16)v;
            }
        }
        cpc->tape_audio_pos = 0;
        cpc->tape_audio_cycles = 0;
        SDL_PutAudioStreamData(cpc->audio_stream,
                               audio_buf, (int)sizeof(audio_buf));
    }
}

void cpc_key_event(CPC *cpc, SDL_Scancode sc, bool pressed) {
    kbd_sdl_key(&cpc->kbd, sc, pressed);
}

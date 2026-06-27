#define _POSIX_C_SOURCE 200112L
#define _FILE_OFFSET_BITS 64
#include "snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Little-endian 16-bit decode */
static u16 le16(const u8 *p) {
    return (u16)p[0] | ((u16)p[1] << 8);
}

#define SNA_CPC_MODEL           0x6D
#define SNA_V3_CRTC_TYPE        0xA4
#define SNA_V3_CRTC_ADDR        0xA5
#define SNA_V3_CRTC_SCANLINE    0xA7
#define SNA_V3_CRTC_CHAR_COUNT  0xA9
#define SNA_V3_CRTC_LINE_COUNT  0xAB
#define SNA_V3_CRTC_RASTER      0xAC
#define SNA_V3_CRTC_VADJUST     0xAD
#define SNA_V3_CRTC_HSW_COUNT   0xAE
#define SNA_V3_CRTC_VSW_COUNT   0xAF
#define SNA_V3_CRTC_FLAGS       0xB0
#define SNA_V3_GA_INT_DELAY     0xB2
#define SNA_V3_GA_SL_COUNT      0xB3
#define SNA_V3_Z80_INT_PENDING  0xB4

static u16 crtc_restore_next_row(const CRTC *crtc, u16 row_start) {
    if (crtc->vlc == crtc->reg[9] && crtc->reg[1] != 0 &&
        crtc->hcc >= crtc->reg[1])
        return (u16)((row_start + crtc->reg[1]) & 0x3FFF);
    return row_start;
}

int snapshot_load(CPC *cpc, const char *path) {
    if (!cpc || cpc->mem.ram_size <= 0) {
        fprintf(stderr, "snapshot: RAM not initialised — refusing to load '%s'\n",
                path ? path : "(null)");
        return -1;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "snapshot: cannot open '%s'\n", path);
        return -1;
    }

    u8 hdr[256];
    if (fread(hdr, 1, 256, f) != 256) {
        fprintf(stderr, "snapshot: '%s' header too short\n", path);
        fclose(f);
        return -1;
    }

    if (memcmp(hdr, "MV - SNA", 8) != 0) {
        fprintf(stderr, "snapshot: '%s' missing 'MV - SNA' signature\n", path);
        fclose(f);
        return -1;
    }

    u8 version = hdr[0x10];
    if (version != 1 && version != 2 && version != 3) {
        fprintf(stderr, "snapshot: '%s' unsupported SNA version %u\n", path, version);
        fclose(f);
        return -1;
    }

    /* ---- Z80 registers ---- */
    cpc->cpu.af  = le16(&hdr[0x11]);
    cpc->cpu.bc  = le16(&hdr[0x13]);
    cpc->cpu.de  = le16(&hdr[0x15]);
    cpc->cpu.hl  = le16(&hdr[0x17]);
    cpc->cpu.r   = hdr[0x19];
    cpc->cpu.i   = hdr[0x1A];
    cpc->cpu.iff1 = hdr[0x1B] != 0;
    cpc->cpu.iff2 = hdr[0x1C] != 0;
    cpc->cpu.ix  = le16(&hdr[0x1D]);
    cpc->cpu.iy  = le16(&hdr[0x1F]);
    cpc->cpu.sp  = le16(&hdr[0x21]);
    cpc->cpu.pc  = le16(&hdr[0x23]);
    cpc->cpu.im  = hdr[0x25];
    cpc->cpu.af_ = le16(&hdr[0x26]);
    cpc->cpu.bc_ = le16(&hdr[0x28]);
    cpc->cpu.de_ = le16(&hdr[0x2A]);
    cpc->cpu.hl_ = le16(&hdr[0x2C]);
    cpc->cpu.halted      = false;
    cpc->cpu.pending_irq = false;
    cpc->cpu.int_accepted = false;
    cpc->cpu.ei_delay    = false;

    /* ---- Gate Array ---- */
    cpc->ga.selected_pen = hdr[0x2E];
    for (int i = 0; i < 17 && i < GA_NUM_INKS; i++)
        cpc->ga.ink[i] = hdr[0x2F + i];

    /* GA mode/ROM byte at 0x40:
     *   bits[1:0] = screen mode (latched, takes effect now)
     *   bit 2     = 1 → lower ROM disabled
     *   bit 3     = 1 → upper ROM disabled  */
    u8 ga_mode = hdr[0x40];
    cpc->ga.requested_mode = ga_mode & 0x03;
    cpc->ga.screen_mode    = ga_mode & 0x03;
    cpc->ga.lower_rom      = !(ga_mode & 0x04);
    cpc->ga.upper_rom      = !(ga_mode & 0x08);
    cpc->mem.lower_rom_enabled = cpc->ga.lower_rom;
    cpc->mem.upper_rom_enabled = cpc->ga.upper_rom;
    cpc->ga.interrupt_counter = 0;
    cpc->ga.interrupt_pending = false;
    cpc->ga.vsync_delay       = 0;
    ga_refresh_palette(&cpc->ga);

    /* GA RAM-cfg byte at 0x41 (lower 6 bits = group<<3 | mode in our ram_bank).
     * Our ram_bank also packs bank_high in bits[7:6]; for standard 128 K SNA
     * (no Yarek extension banks) bank_high stays 0. */
    cpc->mem.ram_bank = hdr[0x41] & 0x3F;

    /* ---- CRTC ---- */
    cpc->crtc.selected = hdr[0x42] & 0x1F;
    for (int i = 0; i < 18 && i < CRTC_NUM_REGS; i++)
        cpc->crtc.reg[i] = hdr[0x43 + i];

    /* ---- Upper ROM select ---- */
    cpc->mem.upper_rom_select = hdr[0x55];

    /* ---- PPI ---- */
    cpc->ppi.port_a = hdr[0x56];
    cpc->ppi.port_b = hdr[0x57];
    cpc->ppi.port_c = hdr[0x58];
    cpc->ppi.control = hdr[0x59];
    cpc->ppi.kbd_row = cpc->ppi.port_c & 0x0F;

    /* ---- PSG ---- */
    cpc->psg.selected = hdr[0x5A];
    /* PSG registers — best-effort: write straight into the reg[] array if
     * the PSG struct exposes one; otherwise the snapshot's PSG state is
     * lost (sound only). We don't poke private PSG state to avoid breaking
     * the synthesiser; the running program will reprogram the PSG anyway. */
    /* (Skipped intentionally — PSG private fields aren't part of public ABI.) */

    /* ---- RAM size (KB) ---- */
    u16 ram_kb = (version >= 2) ? le16(&hdr[0x6B]) : 64;
    if (ram_kb == 0) ram_kb = 64;   /* v1 default */

    if (version >= 3) {
        u8 type = hdr[SNA_V3_CRTC_TYPE];
        if (type <= CRTC_TYPE_3)
            crtc_set_type(&cpc->crtc, (CrtcType)type);

        u16 row_start = le16(&hdr[SNA_V3_CRTC_ADDR]) & 0x3FFF;
        cpc->crtc.hcc = hdr[SNA_V3_CRTC_CHAR_COUNT];
        cpc->crtc.vcc = hdr[SNA_V3_CRTC_LINE_COUNT] & 0x7F;
        cpc->crtc.vlc = hdr[SNA_V3_CRTC_RASTER] & 0x1F;
        cpc->crtc.vac = hdr[SNA_V3_CRTC_VADJUST] & 0x1F;
        cpc->crtc.hsc = hdr[SNA_V3_CRTC_HSW_COUNT] & 0x0F;
        cpc->crtc.vsc = hdr[SNA_V3_CRTC_VSW_COUNT] & 0x0F;

        u16 flags = le16(&hdr[SNA_V3_CRTC_FLAGS]);
        cpc->crtc.vsync = (flags & 0x0001) != 0;
        cpc->crtc.hsync = (flags & 0x0002) != 0;
        cpc->crtc.in_vadjust = (flags & 0x0080) != 0;

        cpc->crtc.ma_row_start = row_start;
        cpc->crtc.ma_next_row = crtc_restore_next_row(&cpc->crtc, row_start);
        cpc->crtc.ma = (u16)((row_start + cpc->crtc.hcc) & 0x3FFF);

        cpc->monitor_vline = le16(&hdr[SNA_V3_CRTC_SCANLINE]);
        cpc->raster_y = cpc->monitor_vline;
        cpc->ga.vsync_delay = hdr[SNA_V3_GA_INT_DELAY] & 0x03;
        cpc->ga.interrupt_counter = hdr[SNA_V3_GA_SL_COUNT];
        cpc->ga.interrupt_pending = false;
        cpc->cpu.pending_irq = hdr[SNA_V3_Z80_INT_PENDING] != 0;
        cpc->prev_hsync = cpc->crtc.hsync;
        cpc->prev_vsync = cpc->crtc.vsync;
        cpc->crtc_cycle_acc = 0;
    } else {
        cpc->crtc.ma_row_start = ((u16)cpc->crtc.reg[12] << 8 | cpc->crtc.reg[13]) & 0x3FFF;
        cpc->crtc.ma_next_row = cpc->crtc.ma_row_start;
        cpc->crtc.ma = cpc->crtc.ma_row_start;
    }
    crtc_recompute_state(&cpc->crtc);
    cpc->crtc_pre_ma = cpc->crtc.ma;
    cpc->crtc_pre_ra = cpc->crtc.vlc;
    cpc->crtc_pre_de = cpc->crtc.display_enable;

    size_t want = (size_t)ram_kb * 1024;
    if (want > (size_t)cpc->mem.ram_size) {
        fprintf(stderr, "snapshot: '%s' wants %u KB RAM, emulator configured for %d KB — "
                        "loading first %d KB only\n",
                path, ram_kb, cpc->mem.ram_size / 1024, cpc->mem.ram_size / 1024);
        want = (size_t)cpc->mem.ram_size;
    }

    /* SNA stores RAM as a flat dump: bytes 0..64KB are the standard main bank,
     * then 64..128KB are the 6128's extra bank, then optional extension banks
     * (group 1, 2, 3...) follow contiguously. Our cpc->mem.ram[] uses the same
     * linear layout so a direct fread works. */
    size_t got = fread(cpc->mem.ram, 1, want, f);
    if (got != want) {
        fprintf(stderr, "snapshot: '%s' RAM short read (%zu / %zu bytes)\n",
                path, got, want);
        fclose(f);
        return -1;
    }
    fclose(f);

    fprintf(stderr, "snapshot: loaded '%s' (v%u, %u KB RAM, PC=%04X SP=%04X)\n",
            path, version, ram_kb, cpc->cpu.pc, cpc->cpu.sp);
    return 0;
}

static void put16(u8 *p, u16 v) { p[0] = v & 0xFF; p[1] = v >> 8; }

int snapshot_save(CPC *cpc, const char *path) {
    u8 hdr[256] = {0};
    memcpy(hdr, "MV - SNA", 8);
    hdr[0x10] = 3;   /* version */

    put16(&hdr[0x11], cpc->cpu.af);
    put16(&hdr[0x13], cpc->cpu.bc);
    put16(&hdr[0x15], cpc->cpu.de);
    put16(&hdr[0x17], cpc->cpu.hl);
    hdr[0x19] = cpc->cpu.r;
    hdr[0x1A] = cpc->cpu.i;
    hdr[0x1B] = cpc->cpu.iff1 ? 1 : 0;
    hdr[0x1C] = cpc->cpu.iff2 ? 1 : 0;
    put16(&hdr[0x1D], cpc->cpu.ix);
    put16(&hdr[0x1F], cpc->cpu.iy);
    put16(&hdr[0x21], cpc->cpu.sp);
    put16(&hdr[0x23], cpc->cpu.pc);
    hdr[0x25] = cpc->cpu.im;
    put16(&hdr[0x26], cpc->cpu.af_);
    put16(&hdr[0x28], cpc->cpu.bc_);
    put16(&hdr[0x2A], cpc->cpu.de_);
    put16(&hdr[0x2C], cpc->cpu.hl_);

    hdr[0x2E] = cpc->ga.selected_pen;
    for (int i = 0; i < 17 && i < GA_NUM_INKS; i++)
        hdr[0x2F + i] = cpc->ga.ink[i];

    hdr[0x40] = (cpc->ga.screen_mode & 0x03)
              | (cpc->ga.lower_rom ? 0 : 0x04)
              | (cpc->ga.upper_rom ? 0 : 0x08);
    hdr[0x41] = cpc->mem.ram_bank & 0x3F;

    hdr[0x42] = cpc->crtc.selected & 0x1F;
    for (int i = 0; i < 18 && i < CRTC_NUM_REGS; i++)
        hdr[0x43 + i] = cpc->crtc.reg[i];

    hdr[0x55] = cpc->mem.upper_rom_select;

    hdr[0x56] = cpc->ppi.port_a;
    hdr[0x57] = cpc->ppi.port_b;
    hdr[0x58] = cpc->ppi.port_c;
    hdr[0x59] = cpc->ppi.control;

    hdr[0x5A] = cpc->psg.selected;
    /* PSG register file isn't part of the public PSG struct ABI; leave
     * 0x5B..0x6A zero. Running programs reprogram the PSG on load. */

    u16 ram_kb = (u16)(cpc->mem.ram_size / 1024);
    put16(&hdr[0x6B], ram_kb);
    hdr[SNA_CPC_MODEL] = (u8)cpc->model;

    hdr[SNA_V3_CRTC_TYPE] = (u8)cpc->crtc.type;
    put16(&hdr[SNA_V3_CRTC_ADDR], cpc->crtc.ma_row_start & 0x3FFF);
    put16(&hdr[SNA_V3_CRTC_SCANLINE], (u16)cpc->monitor_vline);
    hdr[SNA_V3_CRTC_CHAR_COUNT] = cpc->crtc.hcc & 0xFF;
    hdr[SNA_V3_CRTC_CHAR_COUNT + 1] = 0;
    hdr[SNA_V3_CRTC_LINE_COUNT] = cpc->crtc.vcc & 0x7F;
    hdr[SNA_V3_CRTC_RASTER] = cpc->crtc.vlc & 0x1F;
    hdr[SNA_V3_CRTC_VADJUST] = cpc->crtc.vac & 0x1F;
    hdr[SNA_V3_CRTC_HSW_COUNT] = cpc->crtc.hsc & 0x0F;
    hdr[SNA_V3_CRTC_VSW_COUNT] = cpc->crtc.vsc & 0x0F;
    u16 flags = 0;
    if (cpc->crtc.vsync) flags |= 0x0001;
    if (cpc->crtc.hsync) flags |= 0x0002;
    if (cpc->crtc.in_vadjust) flags |= 0x0080;
    put16(&hdr[SNA_V3_CRTC_FLAGS], flags);
    hdr[SNA_V3_GA_INT_DELAY] = cpc->ga.vsync_delay & 0x03;
    hdr[SNA_V3_GA_SL_COUNT] = cpc->ga.interrupt_counter;
    hdr[SNA_V3_Z80_INT_PENDING] = cpc->cpu.pending_irq ? 1 : 0;

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "snapshot: cannot create '%s'\n", path);
        return -1;
    }
    if (fwrite(hdr, 1, 256, f) != 256 ||
        fwrite(cpc->mem.ram, 1, (size_t)cpc->mem.ram_size, f) != (size_t)cpc->mem.ram_size) {
        fprintf(stderr, "snapshot: write to '%s' failed\n", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    fprintf(stderr, "snapshot: saved '%s' (%u KB, PC=%04X SP=%04X)\n",
            path, ram_kb, cpc->cpu.pc, cpc->cpu.sp);
    return 0;
}

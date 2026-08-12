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

static u32 le32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) |
           ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

#define SNA_CPC_MODEL           0x6D
#define SNA_PSG_SELECT          0x5A
#define SNA_PSG_REGS            0x5B
#define SNA_V3_FDC_MOTOR        0x9C
#define SNA_V3_DRVA_TRACK       0x9D
#define SNA_V3_DRVB_TRACK       0x9E
#define SNA_V3_PSG_ENV_STEP     0xA2
#define SNA_V3_PSG_ENV_DIR      0xA3
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

static int memory_chunk_index(const u8 id[4]) {
    if (memcmp(id, "MEM", 3) != 0) return -1;
    if (id[3] >= '0' && id[3] <= '9') return id[3] - '0';
    if (id[3] >= 'A' && id[3] <= 'F') return 10 + id[3] - 'A';
    if (id[3] >= 'a' && id[3] <= 'f') return 10 + id[3] - 'a';
    return -1;
}

static int decode_memory_chunk(u8 *destination, const u8 *data, size_t len) {
    if (len == 0x10000) {
        memcpy(destination, data, len);
        return 0;
    }
    size_t src = 0, dst = 0;
    while (src < len && dst < 0x10000) {
        u8 value = data[src++];
        if (value != 0xE5) {
            destination[dst++] = value;
            continue;
        }
        if (src >= len) return -1;
        u8 count = data[src++];
        if (count == 0) {
            destination[dst++] = 0xE5;
            continue;
        }
        if (src >= len || dst + count > 0x10000) return -1;
        memset(destination + dst, data[src++], count);
        dst += count;
    }
    return src == len && dst == 0x10000 ? 0 : -1;
}

static int load_chunks(FILE *f, CPC *cpc, const char *path,
                       bool memory_required, size_t *memory_size) {
    bool saw_memory = false;
    for (;;) {
        u8 header[8];
        size_t got = fread(header, 1, sizeof(header), f);
        if (got == 0 && feof(f)) break;
        if (got != sizeof(header)) {
            fprintf(stderr, "snapshot: '%s' has a truncated chunk header\n", path);
            return -1;
        }
        u32 length = le32(header + 4);
        int mem_index = memory_chunk_index(header);
        bool known = mem_index >= 0 || memcmp(header, "REMU", 4) == 0;
        if (!known) {
            u8 discard[4096];
            u32 remaining = length;
            while (remaining) {
                size_t count = remaining < sizeof(discard) ? remaining
                                                            : sizeof(discard);
                if (fread(discard, 1, count, f) != count) {
                    fprintf(stderr, "snapshot: '%s' has a truncated %.4s chunk\n",
                            path, header);
                    return -1;
                }
                remaining -= (u32)count;
            }
            continue;
        }
        if (length > 16u * 1024u * 1024u) {
            fprintf(stderr, "snapshot: '%s' chunk %.4s is unreasonably large\n",
                    path, header);
            return -1;
        }
        u8 *payload = length ? malloc(length) : NULL;
        if (length && (!payload || fread(payload, 1, length, f) != length)) {
            fprintf(stderr, "snapshot: '%s' has a truncated %.4s chunk\n",
                    path, header);
            free(payload);
            return -1;
        }
        if (mem_index >= 0) {
            size_t offset = (size_t)mem_index * 0x10000;
            if (offset + 0x10000 > RAM_SIZE ||
                    decode_memory_chunk(cpc->mem.ram + offset,
                                        payload, length) != 0) {
                fprintf(stderr, "snapshot: '%s' has an invalid %.4s memory chunk\n",
                        path, header);
                free(payload);
                return -1;
            }
            if (*memory_size < offset + 0x10000)
                *memory_size = offset + 0x10000;
            saw_memory = true;
        } else if (remu_parse_chunk(cpc, payload, length) < 0) {
            fprintf(stderr, "snapshot: '%s' REMU chunk is out of memory\n", path);
            free(payload);
            return -1;
        }
        free(payload);
    }
    if (memory_required && !saw_memory) {
        fprintf(stderr, "snapshot: '%s' has no RAM dump or MEMx chunks\n", path);
        return -1;
    }
    return 0;
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
    ppi_set_port_a_input(&cpc->ppi, 0xFF);
    ppi_refresh_outputs(&cpc->ppi);

    /* ---- PSG ---- */
    psg_load_registers(&cpc->psg, &hdr[SNA_PSG_REGS], hdr[SNA_PSG_SELECT],
                       version >= 3, hdr[SNA_V3_PSG_ENV_STEP],
                       hdr[SNA_V3_PSG_ENV_DIR]);

    /* ---- RAM size (KB) ---- */
    u16 ram_kb = (version >= 2) ? le16(&hdr[0x6B]) : 64;
    bool chunked_memory = version >= 3 && ram_kb == 0;
    if (version < 3 && ram_kb == 0) ram_kb = 64;

    if (version >= 3) {
        cpc->fdc.motor = hdr[SNA_V3_FDC_MOTOR] != 0;
        cpc->drive[0].cur_track = hdr[SNA_V3_DRVA_TRACK] < DISK_MAX_TRACKS
                                ? hdr[SNA_V3_DRVA_TRACK] : DISK_MAX_TRACKS - 1;
        cpc->drive[1].cur_track = hdr[SNA_V3_DRVB_TRACK] < DISK_MAX_TRACKS
                                ? hdr[SNA_V3_DRVB_TRACK] : DISK_MAX_TRACKS - 1;

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
        cpc->raster_y = 0;
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
    if (version >= 3) {
        /* SNA v3 does not store the CRTC's delayed display-timing latch.
         * Caprice32 loads the CRTC registers while the counters are still at
         * reset, then restores the counters, so display timing remains enabled
         * until the next hardware comparator event. Recomputing it directly
         * from the restored counters blanks Batman Forever's mid-frame
         * snapshots too early. */
        cpc->crtc.h_display = cpc->crtc.reg[1] != 0;
        cpc->crtc.v_display = cpc->crtc.reg[6] != 0;
        cpc->crtc.display_enable = cpc->crtc.h_display && cpc->crtc.v_display;
    }
    cpc->crtc_pre_ma = cpc->crtc.ma;
    cpc->crtc_pre_ra = cpc->crtc.vlc;
    cpc->crtc_pre_de = cpc->crtc.display_enable;

    size_t want = (size_t)ram_kb * 1024;
    if (want > RAM_SIZE) {
        fprintf(stderr, "snapshot: '%s' wants %u KB RAM, emulator supports %d KB max\n",
                path, ram_kb, RAM_SIZE / 1024);
        fclose(f);
        return -1;
    }
    if (!chunked_memory && want > (size_t)cpc->mem.ram_size) {
        fprintf(stderr, "snapshot: '%s' wants %u KB RAM, expanding from %d KB\n",
                path, ram_kb, cpc->mem.ram_size / 1024);
        cpc->mem.ram_size = (int)want;
    }

    remu_debug_clear(&cpc->remu_debug);
    cpc_breakpoint_clear_source(cpc, CPC_BP_SOURCE_SNAPSHOT);

    if (chunked_memory) {
        memset(cpc->mem.ram, 0, sizeof(cpc->mem.ram));
        want = 0;
    } else {
        /* Flat SNA memory uses the same linear physical layout as Mem.ram. */
        size_t got = fread(cpc->mem.ram, 1, want, f);
        if (got != want) {
            fprintf(stderr, "snapshot: '%s' RAM short read (%zu / %zu bytes)\n",
                    path, got, want);
            fclose(f);
            return -1;
        }
    }
    if (load_chunks(f, cpc, path, chunked_memory, &want) < 0) {
        fclose(f);
        return -1;
    }
    fclose(f);

    if (chunked_memory) {
        cpc->mem.ram_size = (int)want;
        ram_kb = (u16)(want / 1024);
    }

    fprintf(stderr, "snapshot: loaded '%s' (v%u, %u KB RAM, PC=%04X SP=%04X)\n",
            path, version, ram_kb, cpc->cpu.pc, cpc->cpu.sp);
    return 0;
}

static void put16(u8 *p, u16 v) { p[0] = v & 0xFF; p[1] = v >> 8; }
static void put32(u8 *p, u32 v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

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

    psg_store_registers(&cpc->psg, &hdr[SNA_PSG_REGS], &hdr[SNA_PSG_SELECT],
                        &hdr[SNA_V3_PSG_ENV_STEP], &hdr[SNA_V3_PSG_ENV_DIR]);

    u16 ram_kb = (u16)(cpc->mem.ram_size / 1024);
    put16(&hdr[0x6B], ram_kb);
    hdr[SNA_CPC_MODEL] = cpc_model_is_plus(cpc->model)
                       ? (u8)MODEL_6128_PLUS : (u8)cpc->model;

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
    hdr[SNA_V3_FDC_MOTOR] = cpc->fdc.motor ? 1 : 0;
    hdr[SNA_V3_DRVA_TRACK] = (u8)cpc->drive[0].cur_track;
    hdr[SNA_V3_DRVB_TRACK] = (u8)cpc->drive[1].cur_track;

    size_t remu_len = 0;
    char *remu = remu_build_chunk(cpc, &remu_len);
    if (remu_len == (size_t)-1) {
        fprintf(stderr, "snapshot: cannot allocate REMU metadata for '%s'\n", path);
        return -1;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "snapshot: cannot create '%s'\n", path);
        free(remu);
        return -1;
    }
    if (fwrite(hdr, 1, 256, f) != 256 ||
        fwrite(cpc->mem.ram, 1, (size_t)cpc->mem.ram_size, f) != (size_t)cpc->mem.ram_size) {
        fprintf(stderr, "snapshot: write to '%s' failed\n", path);
        fclose(f);
        free(remu);
        return -1;
    }
    if (remu_len) {
        u8 chunk_header[8];
        memcpy(chunk_header, "REMU", 4);
        put32(chunk_header + 4, (u32)remu_len);
        if (fwrite(chunk_header, 1, sizeof(chunk_header), f) != sizeof(chunk_header) ||
                fwrite(remu, 1, remu_len, f) != remu_len) {
            fprintf(stderr, "snapshot: REMU write to '%s' failed\n", path);
            fclose(f);
            free(remu);
            return -1;
        }
    }
    fclose(f);
    free(remu);
    fprintf(stderr, "snapshot: saved '%s' (%u KB, PC=%04X SP=%04X)\n",
            path, ram_kb, cpc->cpu.pc, cpc->cpu.sp);
    return 0;
}

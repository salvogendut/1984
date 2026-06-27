#include "cpc.h"
#include "net4cpc.h"
#include "snapshot.h"
#include "kbd_pty.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Set to 1 at runtime to trace CRTC/GA writes to stderr */
int cpc_trace_io = 0;
/* Counter incremented by ide_write on each command, used by crash trace arming */
u32 ide_cmd_count_for_crash_trace = 0;
int cpc_trace_palette = 0;
int cpc_frame_count = 0;
long long g_total_t = 0;
int cpc_trace_input = 0;

/* Ring buffer of recent Z80 PCs for the #102 reset trace. Filled in
 * cpc_frame() before each instruction step; dumped when the firmware
 * cold-start writes $B8BF=0 at PC=$0644 (kernel-initiated reboot). */
u16 g_reset_pc_ring[8192];
u16 g_reset_sp_ring[8192];
u8  g_reset_bank_ring[8192];
u32 g_reset_pc_idx = 0;

/* ---- #102 / debug instrumentation master gate -----------------------------
 * All trace machinery in this file (bus_mem_write hooks, PC-ring fill,
 * BDF4 / FE58 / IDE / runaway / panic detectors) is opt-in via env vars
 * starting with ONE_K_TRACE_ or ONE_K_RESET_. To make sure debug code
 * costs absolutely nothing in production runs, every per-instruction
 * trace site starts with a check against `g_trace_any`. It's computed
 * once at first use by scanning a known list of trace env-vars.
 *
 *   ONE_K_TRACE_BDF4      — kernel panic-stub install + 8K PC ring + runaway
 *   ONE_K_TRACE_FE58      — kernel saved-banking var + GA banking writes
 *   ONE_K_TRACE_IDE_R     — every IDE register read (data port skipped)
 *   ONE_K_TRACE_IDE       — every IDE register write
 *   ONE_K_TRACE_IDE_ISR   — IDE r/w when PC is in kernel-ISR range
 *   ONE_K_TRACE_PANIC     — fire ring dump on PC=$BE54-like halt loop
 *   ONE_K_TRACE_CRASH     — older firmware-reset PC ring (cpc.c bottom)
 *   ONE_K_TRACE_IRQ       — log every IM1 acceptance via cpc.c side
 *   ONE_K_TRACE_BANK      — log every RAM-banking write to GA
 *   ONE_K_TRACE_HDCPM     — log HDCPM ROM activation
 *   ONE_K_TRACE_LDIR_BANK — log LDIR with banked target
 *   ONE_K_RESET_SNA       — auto-snapshot on runaway detector
 *
 * If none of these are set at startup, no trace site does anything.
 * ------------------------------------------------------------------------- */
int g_trace_any = -1;   /* -1 = uninitialised, 0 = no trace, 1 = some trace */
/* Master debug enable, set from cfg->debug by main.c at startup. When 0
 * (default), trace_check_master() forces g_trace_any=0 and per-step env
 * checks (CAP_TEXT, TRACE_LDIR, DUMP_PC, …) all read it as 0, so no
 * debug machinery runs regardless of which ONE_K_* env vars are set. */
int g_debug_enabled = 0;
static const char *const g_trace_vars[] = {
    "ONE_K_TRACE_BDF4", "ONE_K_TRACE_FE58", "ONE_K_TRACE_IDE",
    "ONE_K_TRACE_IDE_R", "ONE_K_TRACE_IDE_ISR", "ONE_K_TRACE_PANIC",
    "ONE_K_TRACE_CRASH", "ONE_K_TRACE_IRQ", "ONE_K_TRACE_BANK",
    "ONE_K_TRACE_HDCPM", "ONE_K_TRACE_LDIR_BANK", "ONE_K_RESET_SNA",
    "ONE_K_TRACE_IM1", "ONE_K_TRACE_PALETTE",
    "ONE_K_CAP_TEXT", "ONE_K_TRACE_LDIR", "ONE_K_TRACE_RTC",
    "ONE_K_TRACE_PSG", "ONE_K_TRACE_AUDIO", "ONE_K_TRACE_FDC",
    NULL
};
static void trace_check_master(void) {
    if (g_trace_any != -1) return;
    g_trace_any = 0;
    if (!g_debug_enabled) return;     /* master gate: debug off → traces stay off */
    for (const char *const *p = g_trace_vars; *p; ++p) {
        if (getenv(*p)) { g_trace_any = 1; break; }
    }
}

#define AUDIO_SAMPLE_RATE   44100
#define AUDIO_SAMPLES_FRAME (AUDIO_SAMPLE_RATE / 50)   /* 882 samples @ 50 Hz */
#define PSG_CLOCK_HZ        1000000                    /* CPC PSG: 4 MHz / 4 */

static const char *fdc_phase_name(FdcPhase phase) {
    switch (phase) {
    case FDC_PHASE_CMD:    return "CMD";
    case FDC_PHASE_EXEC:   return "EXEC";
    case FDC_PHASE_RESULT: return "RESULT";
    }
    return "?";
}

static void trace_fdc_status(CPC *cpc, u16 port, u8 result) {
    if (!dbg_getenv("ONE_K_TRACE_FDC"))
        return;

    static bool have_last = false;
    static u8 last_result = 0;
    static FdcPhase last_phase = FDC_PHASE_CMD;
    static int last_cmd_pos = 0;
    static int last_exec_pos = 0;
    static int last_result_pos = 0;
    static int repeat = 0;

    bool changed = !have_last
                || result != last_result
                || cpc->fdc.phase != last_phase
                || cpc->fdc.cmd_pos != last_cmd_pos
                || cpc->fdc.exec_pos != last_exec_pos
                || cpc->fdc.result_pos != last_result_pos;
    if (!changed) {
        if (++repeat == 10000) {
            fprintf(stderr,
                    "[FDC R f%d] status %04X -> %02X repeated 10000 times PC=%04X phase=%s\n",
                    cpc_frame_count, port, result, cpc->cpu.pc,
                    fdc_phase_name(cpc->fdc.phase));
            repeat = 0;
        }
        return;
    }

    if (have_last && repeat > 0) {
        fprintf(stderr,
                "[FDC R f%d] previous status %02X repeated %d times phase=%s\n",
                cpc_frame_count, last_result, repeat, fdc_phase_name(last_phase));
    }
    fprintf(stderr,
            "[FDC R f%d] status %04X -> %02X PC=%04X phase=%s cmd=%d/%d exec=%d/%d result=%d/%d\n",
            cpc_frame_count, port, result, cpc->cpu.pc,
            fdc_phase_name(cpc->fdc.phase), cpc->fdc.cmd_pos, cpc->fdc.cmd_len,
            cpc->fdc.exec_pos, cpc->fdc.exec_len,
            cpc->fdc.result_pos, cpc->fdc.result_len);

    have_last = true;
    last_result = result;
    last_phase = cpc->fdc.phase;
    last_cmd_pos = cpc->fdc.cmd_pos;
    last_exec_pos = cpc->fdc.exec_pos;
    last_result_pos = cpc->fdc.result_pos;
    repeat = 0;
}

static void trace_fdc_data_read(CPC *cpc, u16 port, u8 result,
                                FdcPhase phase_before, int exec_pos_before,
                                int result_pos_before) {
    if (!dbg_getenv("ONE_K_TRACE_FDC"))
        return;

    bool log = phase_before != FDC_PHASE_EXEC
            || cpc->fdc.phase != phase_before
            || exec_pos_before < 8
            || (cpc->fdc.exec_len > 0 && exec_pos_before >= cpc->fdc.exec_len - 8);
    if (!log)
        return;

    fprintf(stderr,
            "[FDC R f%d] data %04X -> %02X PC=%04X phase=%s->%s exec=%d->%d/%d result=%d->%d/%d\n",
            cpc_frame_count, port, result, cpc->cpu.pc,
            fdc_phase_name(phase_before), fdc_phase_name(cpc->fdc.phase),
            exec_pos_before, cpc->fdc.exec_pos, cpc->fdc.exec_len,
            result_pos_before, cpc->fdc.result_pos, cpc->fdc.result_len);
}

static void trace_fdc_data_write(CPC *cpc, u16 port, u8 val,
                                 FdcPhase phase_before, int cmd_pos_before,
                                 int exec_pos_before) {
    if (!dbg_getenv("ONE_K_TRACE_FDC"))
        return;

    fprintf(stderr,
            "[FDC W f%d] data %04X <- %02X PC=%04X phase=%s->%s cmd=%d->%d/%d exec=%d->%d/%d",
            cpc_frame_count, port, val, cpc->cpu.pc,
            fdc_phase_name(phase_before), fdc_phase_name(cpc->fdc.phase),
            cmd_pos_before, cpc->fdc.cmd_pos, cpc->fdc.cmd_len,
            exec_pos_before, cpc->fdc.exec_pos, cpc->fdc.exec_len);
    if (cpc->fdc.cmd_len > 0) {
        fprintf(stderr, " bytes:");
        for (int i = 0; i < cpc->fdc.cmd_len && i < 9; i++)
            fprintf(stderr, " %02X", cpc->fdc.cmd[i]);
    }
    fprintf(stderr, "\n");
}

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
    /* Bus bypass only applies while M4ROM is actually paged in.
     * The real SymbOS daemon flips slot 6 around its banked M4 calls, so
     * keeping the overlay active after that risks leaking response bytes
     * into CPC screen RAM. */
    bool bypass_slot = cpc->mem.upper_rom_enabled
                       && cpc->mem.upper_rom_select == M4_ROM_SLOT;
    if (cpc->m4 && bypass_slot) {
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
            if (m4_trace
                    && (addr <= 0xE807
                        || (addr >= 0xE8BE && addr <= 0xE8C6)
                        || (addr >= 0xF310 && addr <= 0xF314))) {
                fprintf(stderr,
                        "[m4 f%d] RAMREAD addr=%04X val=%02X PC=%04X SP=%04X "
                        "slot=%u mode=%d left=%d\n",
                        cpc_frame_count, addr, v, cpc->cpu.pc, cpc->cpu.sp,
                        cpc->mem.upper_rom_select, cpc->m4_card.ram_mode,
                        cpc->m4_card.ram_mode_reads);
            }
        }
        else if (addr >= 0xF400 && addr < 0xF500)
            v = cpc->m4_card.cfg_mem[addr - 0xF400];
        else if (addr >= 0xFE00 && addr < 0xFF00)
            v = cpc->m4_card.sock_mem[addr - 0xFE00];
        else
            hit = false;
        if (hit) {
            return v;
        }
    }
    return mem_read(&cpc->mem, addr);
}
static void bus_mem_write(void *ctx, u16 addr, u8 val) {
    CPC *cpc = ctx;
    mem_write(&cpc->mem, addr, val);
    /* Master gate: when no trace env is set, skip every debug hook
     * below. ~1 cycle's worth of branch on the hot path when off. */
    trace_check_master();
    if (!g_trace_any) return;
    /* When palette tracing, log all writes to the firmware workspace area
     * (0xB700-0xB7FF) so we can find the real dirty-flag address on each model. */
    /* When tracing, log writes to suspected 464 palette buffer B1D9-B1FC. */
    if (cpc_trace_palette && cpc_frame_count > 520
            && addr >= 0xB1D9 && addr <= 0xB1FC)
        fprintf(stderr, "[f%04d memw] %04X <- %02X  lrom=%d\n",
                cpc_frame_count, addr, val, cpc->mem.lower_rom_enabled);
    /* #102 wedge: trace writes to the firmware's secondary-tick vector
     * slot at $BDF4..$BDF6 and to the counter at $B8BF, so we can see
     * exactly when (and from where) the panic placeholder gets either
     * overwritten by the kernel or kept in place. */
    static int trace_bdf4 = -1;
    if (trace_bdf4 == -1) trace_bdf4 = dbg_getenv("ONE_K_TRACE_BDF4") ? 1 : 0;
    if (trace_bdf4 && (addr >= 0xBDF4 && addr <= 0xBDF6))
        fprintf(stderr, "[BDF4w] frame=%d  $%04X <- %02X  PC=%04X  lrom=%d\n",
                cpc_frame_count, addr, val, cpc->cpu.pc, cpc->mem.lower_rom_enabled);
    /* Snapshot the system state the moment we detect the reset, so
     * we can disassemble the runaway area off-line. Trigger same as
     * the [RESET] dump below; gated on env ONE_K_RESET_SNA=path. */
    /* Reset signal: firmware reinstalls $BDF4=C3 from PC=$0ABE with
     * lrom=1 (the firmware tick-vector setup), AFTER the kernel had
     * patched $BDF5=4F (= JP $BE4F). Catches the kernel reset moment
     * deterministically. */
    static u8 prev_bdf5 = 0;
    if (trace_bdf4 && addr == 0xBDF4 && val == 0xC3
            && cpc->cpu.pc == 0x0ABE && cpc->mem.lower_rom_enabled
            && prev_bdf5 == 0x4F) {
        extern u16 g_reset_pc_ring[8192];
        extern u32 g_reset_pc_idx;
        static int reset_dumped = 0;
        if (!reset_dumped && g_reset_pc_idx > 256) {
            reset_dumped = 1;
            fprintf(stderr, "[RESET] frame=%d — kernel rebooted (firmware "
                            "reinit at $0ABE). Last 8192 PCs leading up:\n",
                    cpc_frame_count);
            /* (Snapshot already saved by the earlier [RUNAWAY] trigger
             * — that captures the pre-corruption state we actually
             * want. Saving again here would overwrite it with the
             * post-reset firmware state.) */
            u32 start = g_reset_pc_idx & 0x1FFF;
            u16 last = 0xFFFF; int run = 0;
            for (int i = 0; i < 8192; i++) {
                u32 j = (start + i) & 0x1FFF;
                u16 pc = g_reset_pc_ring[j];
                if (pc == last) { run++; continue; }
                if (run > 0) fprintf(stderr, "        (x%d more)\n", run);
                run = 0; last = pc;
                fprintf(stderr, "  PC=%04X  SP=%04X  bank=%02X\n", pc, g_reset_sp_ring[j], g_reset_bank_ring[j]);
            }
            fflush(stderr);
        }
    }
    if (trace_bdf4 && addr == 0xBDF5) prev_bdf5 = val;
    if (trace_bdf4 && addr == 0xB8BF) {
        fprintf(stderr, "[B8BFw] frame=%d  $B8BF <- %02X  PC=%04X  lrom=%d\n",
                cpc_frame_count, val, cpc->cpu.pc, cpc->mem.lower_rom_enabled);
    }
    /* The actual target of JP $BE4F. If $BE4F holds 'LD A,$FF; LD
     * ($FFF3),A; JP $BE4F' (3E FF 32 F3 FF C3 4F BE) we panic when
     * called; the kernel is supposed to patch this slot with a real
     * RET-bearing routine before the next 50 Hz secondary tick. */
    if (trace_bdf4 && (addr >= 0xBE4F && addr <= 0xBE56))
        fprintf(stderr, "[BE4Fw] frame=%d  $%04X <- %02X  PC=%04X HL=%04X DE=%04X BC=%04X  lrom=%d\n",
                cpc_frame_count, addr, val, cpc->cpu.pc,
                cpc->cpu.hl, cpc->cpu.de, cpc->cpu.bc,
                cpc->mem.lower_rom_enabled);
    /* Also flag any write to the kernel error byte $FFF3 — that's
     * what the halt loop writes to, so if we see 'FF' at $FFF3 it
     * means we entered the panic at least once. */
    if (trace_bdf4 && addr == 0xFFF3)
        fprintf(stderr, "[FFF3w] frame=%d  $FFF3 <- %02X  PC=%04X  lrom=%d\n",
                cpc_frame_count, val, cpc->cpu.pc, cpc->mem.lower_rom_enabled);
    /* #102 layer A: trace writes to $FE58, the kernel's saved-banking
     * variable. The kernel writes this from inside its ISR via the
     * routine at $FD04 (PUSH BC; LD ($FE58),A; ADD A,$C1; OUT (C),A;
     * POP BC; RET). On ISR exit the kernel restores banking from
     * $FE58 via $FDA4 (LD A,($FE58); CALL $FD04). If a nested IRQ
     * clobbers $FE58 between save and restore, the kernel ends up in
     * the wrong bank and JPs to garbage. Gated on ONE_K_TRACE_FE58
     * so we don't interfere with quiet runs. */
    static int trace_fe58 = -1;
    if (trace_fe58 == -1) trace_fe58 = dbg_getenv("ONE_K_TRACE_FE58") ? 1 : 0;
    if (trace_fe58 && addr == 0xFE58)
        fprintf(stderr, "[FE58w] frame=%d  $FE58 <- %02X  PC=%04X  ram_bank=%02X  lrom=%d\n",
                cpc_frame_count, val, cpc->cpu.pc, cpc->mem.ram_bank,
                cpc->mem.lower_rom_enabled);
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
    /* USIfAC II serial @ 0xFBD0..FBDF. Decoded before the FDC clause so
     * even if the FDC's lo-bit-7 gate ever changes, USIfAC keeps its
     * range. Internally dispatches on the low nibble (D0/D1/D8/DD). */
    else if (cpc->mx4 && cpc->usifac.present && hi == 0xFB &&
             (port & 0xF0) == 0xD0) {
        result = usifac_read(&cpc->usifac, (u8)(port & 0x0F));
    }
    /* FDC: hi=0xFB AND lo bit 7=0 → status (lo bit 0=0) or data (lo bit 0=1).
     * Real CPC hardware decodes the FDC at 0xFB7E/0xFB7F only; the upper
     * half (0xFB80–0xFBFF) is free for peripherals like USIfAC at
     * 0xFBD0/D1. Without the lo-bit-7 gate, generic IN A,(0xFBxx) probes
     * from other software read the FDC main status register instead of
     * 0xFF and mistake the FDC for a different device. */
    else if (hi == 0xFB && !(port & 0x80)) {
        u8 lo = port & 0xFF;
        if (lo & 0x01) {
            FdcPhase phase_before = cpc->fdc.phase;
            int exec_pos_before = cpc->fdc.exec_pos;
            int result_pos_before = cpc->fdc.result_pos;
            result = fdc_read_data(&cpc->fdc);
            trace_fdc_data_read(cpc, port, result, phase_before,
                                exec_pos_before, result_pos_before);
        } else {
            result = fdc_read_status(&cpc->fdc);
            trace_fdc_status(cpc, port, result);
        }
    }
    else if (hi == 0xFA) {
        result = 0xFF;
    }
    /* Albireo CH376: hi=0xFE, lo=0x80/0x81 (left chip, storage or USB
     * mouse) or 0x40/0x41 (right chip, dual-mode storage).
     * Claim before M4 wide decode. */
    else if (cpc->mx4 && cpc->albireo && hi == 0xFE && (port & 0xFE) == 0x80) {
        result = ch376_read(&cpc->ch376, (u8)(port & 0x01));
    }
    else if (cpc->mx4 && cpc->albireo && cpc->albireo_mouse &&
             hi == 0xFE && (port & 0xFE) == 0x40) {
        result = ch376_read(&cpc->ch376_b, (u8)(port & 0x01));
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
        else if (cpc->symbiface_ide && (lo == 0x06 || (lo >= 0x08 && lo <= 0x0F))) {
            result = ide_read(&cpc->ide_chip, lo);
            if (dbg_getenv("ONE_K_TRACE_IDE_R") && lo != 0x08) {
                fprintf(stderr, "[IDE R] port=FD%02X -> %02X PC=%04X\n", lo, result, cpc->cpu.pc);
            }
            /* Wider trace for the #102 kernel-ISR investigation: log
             * every read INCLUDING the data port, gated on PC being
             * in the kernel-ISR IDE block ($FD00-$FDAF) or close to
             * a known-bad address. Helps diff failing vs passing runs. */
            if (dbg_getenv("ONE_K_TRACE_IDE_ISR") && cpc->cpu.pc >= 0xFD00
                    && cpc->cpu.pc <= 0xFDAF) {
                fprintf(stderr, "[IDE-ISR R] frame=%d port=FD%02X -> %02X PC=%04X SP=%04X\n",
                        cpc_frame_count, lo, result, cpc->cpu.pc, cpc->cpu.sp);
            }
        }
        else if (cpc->rtc && lo == 0x14) {
            result = rtc_read_data(&cpc->rtc_chip);
            if (dbg_getenv("ONE_K_TRACE_RTC")) {
                fprintf(stderr, "[RTC R] frame=%d addr=%02X -> %02X PC=%04X\n",
                        cpc_frame_count, cpc->rtc_chip.addr, result, cpc->cpu.pc);
            }
        }
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

    /* Parallel printer port: any write with A12 LOW (0xEFxx). The
     * printer module handles the strobe-edge detect and bit-7 invert.
     * Side-effect only — fall through so any expansion that happens to
     * decode the same address space (none in the stock CPC, but the
     * decoder is permissive) still sees the write. Caprice32
     * cap32.cpp:768. */
    if (!(hi & 0x10))
        printer_out(&cpc->printer, val);

    /* Gate Array: A15=0, A14=1 → 0x7Fxx */
    if (!(hi & 0x80) && (hi & 0x40)) {
        if (cpc_trace_palette && cpc_frame_count > 520 && (val & 0xC0) == 0x40)
            fprintf(stderr, "[f%04d ga] pen=%02X col=%02X\n",
                    cpc_frame_count, cpc->ga.selected_pen, val & 0x1F);
        else if (cpc_trace_palette && cpc_frame_count > 520
                 && (val & 0xC0) == 0x00 && val <= 0x10)
            fprintf(stderr, "[f%04d ga] select pen=%02X\n",
                    cpc_frame_count, val);
        /* #102 layer A: trace every RAM-banking write (top 2 bits = 11)
         * so we can correlate banking flips against $FE58 writes and
         * IRQ events. */
        if (dbg_getenv("ONE_K_TRACE_FE58") && (val & 0xC0) == 0xC0)
            fprintf(stderr, "[GA-bank] frame=%d  val=%02X  PC=%04X  was=%02X\n",
                    cpc_frame_count, val, cpc->cpu.pc, cpc->mem.ram_bank);
        /* Interrupt reset also cancels a request already latched by the CPU. */
        if ((val & 0xC0) == 0x80 && (val & 0x10))
            cpc->cpu.pending_irq = false;
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
        if ((val & 0xC0) == 0xC0 && cpc->mem.ram_size > 0x10000) {
            u8 bank_high = ((hi & 0xFC) == 0x7C) ? ((~hi) & 0x03) : 0;
            u8 mode  = val & 0x07;
            u8 group = (val >> 3) & 0x07;
            /* Caprice32 ga_memory_manager() quirk: when the selected
             * expansion group would point past installed RAM, force
             * group=0 (fall back to the first 64K extension bank).
             * Without this, CP/M+ and other software that probes more
             * banks than physically exist see 0xFF reads where real
             * hardware mirrors to group 0. Materially affects smaller
             * configs (128/192/256/384/448K); a no-op at 576K/1024K
             * where all standard groups fit. */
            u32 full_bg = (u32)bank_high * 8u + (u32)group;
            if (((full_bg + 2u) * 64u * 1024u) > (u32)cpc->mem.ram_size) {
                group     = 0;
                bank_high = 0;
            }
            cpc->mem.ram_bank = (u8)((bank_high << 6) | (group << 3) | mode);
            if (dbg_getenv("ONE_K_TRACE_BANK"))
                fprintf(stderr, "[BANK] ram_bank=%02X (group=%u mode=%u bank_high=%u) PC=%04X\n",
                        cpc->mem.ram_bank, group, mode, bank_high, cpc->cpu.pc);
        }
        return;
    }
    /* CRTC write functions: A14=0 AND A9=0. A8 selects:
     *   A8=0 → 0xBCxx select, A8=1 → 0xBDxx write.
     * A9=1 (0xBExx/0xBFxx) is the CRTC read side — OUT to those ports
     * is a no-op on real hardware. FUZIX's SDCC port helper at
     * F995 issues OUT (C),B with BC=0x03FF for unrelated reasons; the
     * old A14-only decode wrongly latched that as R12 := 0x03 and
     * pointed the screen at block 0. */
    if (!(hi & 0x40) && !(hi & 0x02)) {
        if (!(hi & 0x01)) {
            crtc_select(&cpc->crtc, val);
        } else {
            if (cpc_trace_io)
                fprintf(stderr, "CRTC R%-2d = %3d (0x%02X)\n",
                        cpc->crtc.selected, val, val);
            if (dbg_getenv("ONE_K_TRACE_CRTC_REGS"))
                fprintf(stderr, "[CRTC] PC=%04X R%-2d = 0x%02X  BC=%04X HL=%04X DE=%04X AF=%04X\n",
                        cpc->cpu.pc, cpc->crtc.selected, val,
                        cpc->cpu.bc, cpc->cpu.hl, cpc->cpu.de, cpc->cpu.af);
            crtc_write(&cpc->crtc, val);
        }
        return;
    }
    /* Upper ROM select: A15=1, A14=1, A13=0 → 0xC0xx–0xDFxx */
    if ((hi & 0xE0) == 0xC0) {
        if (dbg_getenv("ONE_K_TRACE_HDCPM") && val == 0x01 && cpc->mem.upper_rom_select != 0x01) {
            fprintf(stderr, "[HDCPM ROM SELECTED] PC=%04X (caller about to use HDCPM ROM)\n", cpc->cpu.pc);
        }
        cpc->mem.upper_rom_select = val;
        return;
    }
    /* PPI: A11=0 → 0xF4 (port A), 0xF5 (B), 0xF6 (C), 0xF7 (ctrl) */
    if (!(hi & 0x08)) {
        u8 ppi_port = hi & 0x03;
        ppi_write(&cpc->ppi, ppi_port, val);
        /* Cassette motor follows PPI port C bit 4. */
        tape_set_motor(&cpc->tape, (cpc->ppi.port_c & 0x10) != 0);
        /* The AY bus is driven only when a write can change PPI port A or
         * upper port C. Port B and mode-set writes must not replay stale
         * BDIR/BC1 levels. */
        bool drives_psg = (ppi_port == 0 && !(cpc->ppi.control & 0x10))
                       || (ppi_port == 2 && !(cpc->ppi.control & 0x08))
                       || (ppi_port == 3 && !(val & 0x80)
                                         && !(cpc->ppi.control & 0x08));
        if (!drives_psg)
            return;

        u8 psg_ctrl = (cpc->ppi.port_c >> 6) & 0x03;
        if (psg_ctrl == 0x03) {
            if (dbg_getenv("ONE_K_TRACE_PSG"))
                fprintf(stderr, "[PSG f%d] select=%02X PC=%04X port=%04X\n",
                        cpc_frame_count, cpc->ppi.port_a, cpc->cpu.pc, port);
            psg_select(&cpc->psg, cpc->ppi.port_a);
        }
        else if (psg_ctrl == 0x02) {
            if (dbg_getenv("ONE_K_TRACE_PSG"))
                fprintf(stderr, "[PSG f%d] R%u <- %02X PC=%04X port=%04X\n",
                        cpc_frame_count, cpc->psg.selected, cpc->ppi.port_a,
                        cpc->cpu.pc, port);
            psg_write(&cpc->psg, cpc->ppi.port_a);
        }
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
    /* Albireo CH376: hi=0xFE, lo=0x80/0x81 (left chip, storage or USB
     * mouse) or 0x40/0x41 (right chip, dual-mode storage).
     * Claim before M4 wide decode. */
    if (cpc->mx4 && cpc->albireo && hi == 0xFE && (port & 0xFE) == 0x80) {
        ch376_write(&cpc->ch376, (u8)(port & 0x01), val);
        return;
    }
    if (cpc->mx4 && cpc->albireo && cpc->albireo_mouse &&
            hi == 0xFE && (port & 0xFE) == 0x40) {
        ch376_write(&cpc->ch376_b, (u8)(port & 0x01), val);
        return;
    }
    /* M4 DATAPORT: hi=0xFE or 0xFF — accumulate command byte */
    if (cpc->mx4 && cpc->m4 && (hi == 0xFE || hi == 0xFF)) {
        m4_dataport_write(&cpc->m4_card, val); return;
    }
    /* M4 ACKPORT: hi=0xFC — trigger command execution */
    if (cpc->mx4 && cpc->m4 && hi == 0xFC) {
        if (dbg_getenv("ONE_K_TRACE_M4_IO")) {
            fprintf(stderr,
                    "[M4ACK f%d] port=%04X val=%02X PC=%04X SP=%04X len=%d head:",
                    cpc_frame_count, port, val, cpc->cpu.pc, cpc->cpu.sp,
                    cpc->m4_card.cmd_len);
            for (int i = 0; i < cpc->m4_card.cmd_len && i < 16; i++)
                fprintf(stderr, " %02X", cpc->m4_card.cmd_buf[i]);
            fprintf(stderr, "\n");
        }
        if (m4_ackport_write(&cpc->m4_card, &cpc->mem))
            z80_nmi(&cpc->cpu);
        return;
    }
    /* FDC motor: hi=0xFA, write */
    if (hi == 0xFA) {
        fdc_motor_write(&cpc->fdc, val);
        if (dbg_getenv("ONE_K_TRACE_FDC"))
            fprintf(stderr, "[FDC W f%d] motor %04X <- %02X PC=%04X on=%d\n",
                    cpc_frame_count, port, val, cpc->cpu.pc, cpc->fdc.motor);
        return;
    }
    /* USIfAC II serial @ 0xFBD0..FBDF (write). */
    if (cpc->mx4 && cpc->usifac.present && hi == 0xFB &&
        (port & 0xF0) == 0xD0) {
        usifac_write(&cpc->usifac, (u8)(port & 0x0F), val);
        return;
    }
    /* FDC data: hi=0xFB, lo bit 7 = 0, write */
    if (hi == 0xFB) {
        u8 lo = port & 0xFF;
        if (!(lo & 0x80)) {
            FdcPhase phase_before = cpc->fdc.phase;
            int cmd_pos_before = cpc->fdc.cmd_pos;
            int exec_pos_before = cpc->fdc.exec_pos;
            fdc_write_data(&cpc->fdc, val);
            trace_fdc_data_write(cpc, port, val, phase_before,
                                 cmd_pos_before, exec_pos_before);
        }
        return;
    }
    /* hi=0xFD: IDE (0xFD06, 0xFD08-0xFD0F), RTC (0xFD14/0xFD15), Net4CPC (0xFD20-0xFD23) */
    if (cpc->mx4 && hi == 0xFD) {
        u8 lo = port & 0xFF;
        if (cpc->symbiface_ide && (lo == 0x06 || (lo >= 0x08 && lo <= 0x0F))) {
            if (dbg_getenv("ONE_K_TRACE_IDE")) {
                fprintf(stderr, "[IDE W] port=FD%02X val=%02X PC=%04X\n", lo, val, cpc->cpu.pc);
            }
            if (dbg_getenv("ONE_K_TRACE_IDE_ISR") && cpc->cpu.pc >= 0xFD00
                    && cpc->cpu.pc <= 0xFDAF) {
                fprintf(stderr, "[IDE-ISR W] frame=%d port=FD%02X val=%02X PC=%04X SP=%04X\n",
                        cpc_frame_count, lo, val, cpc->cpu.pc, cpc->cpu.sp);
            }
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

            if (lo == 0x3F) {
                /* The bank-aware helper consumed the response without going
                 * through bus_mem_read(), so retire RAM mode explicitly. */
                if (m4_trace) {
                    fprintf(stderr,
                            "[m4 f%d] HRECV src=%04X dst=%04X len=%u "
                            "srcbank=%02X dstbank=%02X ret=%04X first=%02X\n",
                            cpc_frame_count, src, dest, length, source_bank,
                            dest_bank, retaddr,
                            (src >= 0xE800 && src < 0xF400)
                                ? cpc->m4_card.bus_mem[src - 0xE800]
                                : mem_read(&cpc->mem, src));
                }
                cpc->m4_card.ram_mode = false;
                cpc->m4_card.ram_mode_reads = 0;
                cpc->m4_card.last_resp_len = 0;
            }

            cpc->cpu.pc = cpc->cpu.ix;
            (void)retaddr;
        }
        return;
    }
}

/* ---- Init / destroy ---- */

static void build_pen_tables(void);
static bool g_pen_tables_built;

static CrtcType default_crtc_type(CpcModel model) {
    switch (model) {
    case MODEL_6128:
        return CRTC_TYPE_1;
    case MODEL_464:
    case MODEL_664:
    default:
        return CRTC_TYPE_0;
    }
}

/* Forward decl — definition lives near cpc_frame() with the rest of
 * the CRTC/bus advance code. */
static void cpc_bus_tick(void *ctx, int cycles);

#define MONITOR_SYNC_DEC_MAX     80
#define MONITOR_SYNC_INC_MAX     80
#define MONITOR_HSYNC_DURATION   0x0A00
#define MONITOR_BASE_HSYNC       (0x4000 - MONITOR_HSYNC_DURATION)
#define MONITOR_SYNC_TOLERANCE   257
#define MONITOR_X_ADJUST_PIXELS  -16
#define MONITOR_MIN_VHOLD        250
#define MONITOR_MIN_VSYNC        295
#define MONITOR_MAX_VSYNC        351

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void cpc_monitor_reset(CPC *cpc) {
    cpc->raster_x = 0;
    cpc->raster_y = 0;
    cpc->monitor_vline = 0;
    cpc->monitor_hpos = 0x0500;
    cpc->monitor_hsync_duration = MONITOR_HSYNC_DURATION;
    cpc->monitor_min_hsync = MONITOR_BASE_HSYNC - MONITOR_SYNC_TOLERANCE;
    cpc->monitor_max_hsync = MONITOR_BASE_HSYNC + MONITOR_SYNC_TOLERANCE;
    cpc->monitor_hsync = MONITOR_BASE_HSYNC;
    cpc->monitor_free_hsync = MONITOR_BASE_HSYNC;
    cpc->monitor_hs_peak_pos = 0;
    cpc->monitor_hs_start_pos = 0;
    cpc->monitor_hs_end_pos = 0;
    cpc->monitor_hs_peak_to_start = 0;
    cpc->monitor_hs_start_to_peak = 0;
    cpc->monitor_hs_end_to_peak = 0;
    cpc->monitor_hs_peak_to_end = 0;
    cpc->monitor_had_peak = false;
    cpc->monitor_in_hsync = false;
    cpc->monitor_frame_completed = false;
}

int cpc_init(CPC *cpc, CpcModel model, const char *rom_os, const char *rom_basic, int scale) {
    if (!g_pen_tables_built) build_pen_tables();
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
    crtc_set_type(&cpc->crtc, default_crtc_type(model));
    cpc_monitor_reset(cpc);
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
    cpc->ch376.tag = "albireo-a";
    ch376_init(&cpc->ch376_b);
    cpc->ch376_b.tag = "albireo-b";
    tape_init(&cpc->tape);
    printer_init(&cpc->printer);
    net4cpc_reset();

    cpc->bus.mem_read       = bus_mem_read;
    cpc->bus.mem_write      = bus_mem_write;
    cpc->bus.io_read        = bus_io_read;
    cpc->bus.io_write       = bus_io_write;
    cpc->bus.tick           = cpc_bus_tick;
    cpc->bus.ticked_in_step = &cpc->bus_ticked_in_step;
    cpc->bus.ctx            = cpc;

    /* OS title is just "1984"; the model name and F-key hints are drawn
     * inside the window each frame (top-left header). */
    (void)model;
    const char *title = "1984";
    if (display_init(&cpc->display, title, scale) < 0)
        return -1;

    SDL_AudioSpec spec = { SDL_AUDIO_S16, 1, AUDIO_SAMPLE_RATE };
    cpc->audio_stream = SDL_OpenAudioDeviceStream(
        SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!cpc->audio_stream) {
        fprintf(stderr, "SDL_OpenAudioDeviceStream: %s\n", SDL_GetError());
    } else if (!SDL_ResumeAudioStreamDevice(cpc->audio_stream)) {
        fprintf(stderr, "SDL_ResumeAudioStreamDevice: %s\n", SDL_GetError());
    } else if (dbg_getenv("ONE_K_TRACE_AUDIO")) {
        fprintf(stderr, "[audio] opened S16 mono %d Hz playback stream\n",
                AUDIO_SAMPLE_RATE);
    }

    return 0;
}

void cpc_reset(CPC *cpc) {
    z80_reset(&cpc->cpu);
    ga_init(&cpc->ga);
    crtc_init(&cpc->crtc);
    crtc_set_type(&cpc->crtc, default_crtc_type(cpc->model));
    ppi_init(&cpc->ppi);
    psg_init(&cpc->psg);
    kbd_init(&cpc->kbd);
    fdc_reset(&cpc->fdc);
    rtc_init(&cpc->rtc_chip);
    ide_reset(&cpc->ide_chip);  /* keeps image file open across warm reset */
    mouse_init(&cpc->mouse);    /* clear accumulated deltas; capture state managed by main */
    m4_reset(&cpc->m4_card);
    ch376_reset(&cpc->ch376);
    ch376_reset(&cpc->ch376_b);
    cpc->mem.lower_rom_enabled = true;
    cpc->mem.upper_rom_enabled = true;
    cpc->mem.ram_bank = 0;
    cpc_monitor_reset(cpc);
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
    ch376_close(&cpc->ch376_b);
    tape_eject(&cpc->tape);
    display_destroy(&cpc->display);
}

/* ---- Pixel rendering ----
 *
 * Each CRTC character clock fetches 2 bytes and outputs 16 pixels.
 * Mode 2: 8 pixels/byte, 1 bit per pixel  → 1:1 mapping (16 px)
 * Mode 1: 4 pixels/byte, 2 bits per pixel → each pixel doubled  (16 px)
 * Mode 0: 2 pixels/byte, 4 bits per pixel → each pixel ×4       (16 px)
 * Mode 3: 2 pixels/byte, 2 bits per pixel → each pixel ×4       (16 px)
 *
 * Video address: A[15:14] = MA[13:12], A[13:11] = RA[2:0], A[10:1] = MA[9:0]
 */

/* Precomputed byte → pen-index tables. Built once at startup; the per-pixel
 * render path then just indexes ga->resolved_ink[] (already-cached u32).
 *
 *   mode0_pens[b] = {left_pen, right_pen}  — 2 pixels × 4 wide each
 *   mode1_pens[b] = 4 pens                 — each pixel doubled
 *   mode2_pens[b] = 8 pens                 — 1:1
 *   mode3_pens[b] = {left_pen, right_pen}  — 2 pixels × 4 wide each
 *
 * Pen-bit layout per the CRTC docs:
 *   mode 0:  pen0 = b1 b5 b3 b7  ; pen1 = b0 b4 b2 b6
 *   mode 1:  pen[i] = b(3-i+4) << 1 | b(3-i+0)
 *   mode 2:  pen[i] = (byte >> (7 - i)) & 1
 *   mode 3:  pen0 = b3 b7        ; pen1 = b2 b6
 */
static u8 g_mode0_pens[256][2];
static u8 g_mode1_pens[256][4];
static u8 g_mode2_pens[256][8];
static u8 g_mode3_pens[256][2];

static void build_pen_tables(void) {
    for (int b = 0; b < 256; b++) {
        u8 pens[8];
        ga_decode_byte(0, (u8)b, pens);
        g_mode0_pens[b][0] = pens[0];
        g_mode0_pens[b][1] = pens[4];
        ga_decode_byte(1, (u8)b, pens);
        for (int p = 0; p < 4; p++) g_mode1_pens[b][p] = pens[p * 2];
        ga_decode_byte(2, (u8)b, pens);
        for (int p = 0; p < 8; p++) g_mode2_pens[b][p] = pens[p];
        ga_decode_byte(3, (u8)b, pens);
        g_mode3_pens[b][0] = pens[0];
        g_mode3_pens[b][1] = pens[4];
    }
    g_pen_tables_built = true;
}

/* Caller (cpc.c frame loop) guarantees px is a multiple of 16 in [0, 752],
 * so px..px+15 always fits in CPC_SCREEN_W (768). No per-pixel bounds
 * checks — the only valid call sites have already gated px. */
static inline void render_char(u32 *row, int px, const GateArray *ga, u8 b0, u8 b1) {
    const u32 *inks = ga->resolved_ink;
    u32 *out = row + px;

    switch (ga->screen_mode) {
    case 2: { /* 1 bpp, 8 pixels/byte */
        const u8 *p0 = g_mode2_pens[b0];
        const u8 *p1 = g_mode2_pens[b1];
        out[0]  = inks[p0[0]]; out[1]  = inks[p0[1]];
        out[2]  = inks[p0[2]]; out[3]  = inks[p0[3]];
        out[4]  = inks[p0[4]]; out[5]  = inks[p0[5]];
        out[6]  = inks[p0[6]]; out[7]  = inks[p0[7]];
        out[8]  = inks[p1[0]]; out[9]  = inks[p1[1]];
        out[10] = inks[p1[2]]; out[11] = inks[p1[3]];
        out[12] = inks[p1[4]]; out[13] = inks[p1[5]];
        out[14] = inks[p1[6]]; out[15] = inks[p1[7]];
        break;
    }
    case 1: { /* 2 bpp, 4 pixels/byte, ×2 */
        const u8 *p0 = g_mode1_pens[b0];
        const u8 *p1 = g_mode1_pens[b1];
        u32 c;
        c = inks[p0[0]]; out[0]  = c; out[1]  = c;
        c = inks[p0[1]]; out[2]  = c; out[3]  = c;
        c = inks[p0[2]]; out[4]  = c; out[5]  = c;
        c = inks[p0[3]]; out[6]  = c; out[7]  = c;
        c = inks[p1[0]]; out[8]  = c; out[9]  = c;
        c = inks[p1[1]]; out[10] = c; out[11] = c;
        c = inks[p1[2]]; out[12] = c; out[13] = c;
        c = inks[p1[3]]; out[14] = c; out[15] = c;
        break;
    }
    case 0: { /* 4 bpp, 2 pixels/byte, ×4 */
        u32 c0 = inks[g_mode0_pens[b0][0]];
        u32 c1 = inks[g_mode0_pens[b0][1]];
        u32 c2 = inks[g_mode0_pens[b1][0]];
        u32 c3 = inks[g_mode0_pens[b1][1]];
        out[0]  = c0; out[1]  = c0; out[2]  = c0; out[3]  = c0;
        out[4]  = c1; out[5]  = c1; out[6]  = c1; out[7]  = c1;
        out[8]  = c2; out[9]  = c2; out[10] = c2; out[11] = c2;
        out[12] = c3; out[13] = c3; out[14] = c3; out[15] = c3;
        break;
    }
    case 3: { /* undocumented 2 bpp, 2 pixels/byte, ×4 */
        u32 c0 = inks[g_mode3_pens[b0][0]];
        u32 c1 = inks[g_mode3_pens[b0][1]];
        u32 c2 = inks[g_mode3_pens[b1][0]];
        u32 c3 = inks[g_mode3_pens[b1][1]];
        out[0]  = c0; out[1]  = c0; out[2]  = c0; out[3]  = c0;
        out[4]  = c1; out[5]  = c1; out[6]  = c1; out[7]  = c1;
        out[8]  = c2; out[9]  = c2; out[10] = c2; out[11] = c2;
        out[12] = c3; out[13] = c3; out[14] = c3; out[15] = c3;
        break;
    }
    }
}

/* ---- Frame execution ----
 *
 * Rendering follows the monitor beam, not the raw CRTC HSYNC edge. The CRTC
 * still drives display enable, interrupts and mode latch, but the monitor has
 * inertia: short/narrow HSYNC pulses can be ignored and legal rupture effects
 * shift the rendered line instead of forcibly starting a new one. */
static void cpc_monitor_start_hsync(CPC *cpc) {
    if (cpc->monitor_in_hsync)
        return;
    cpc->monitor_in_hsync = true;
    cpc->monitor_hs_start_pos = 0;
    cpc->monitor_hs_peak_to_start = cpc->monitor_hs_peak_pos;
}

static void cpc_monitor_end_hsync(CPC *cpc) {
    int temp;

    if (!cpc->monitor_in_hsync)
        return;

    cpc->monitor_in_hsync = false;
    cpc->monitor_hs_peak_to_end = cpc->monitor_hs_peak_pos;

    if (!cpc->monitor_had_peak) {
        cpc->monitor_hs_end_pos = 0;
        return;
    }

    cpc->monitor_had_peak = false;
    if (cpc->monitor_hs_peak_pos >= cpc->monitor_hs_start_pos) {
        temp = cpc->monitor_hs_end_pos - cpc->monitor_hsync_duration;
        if (temp < cpc->monitor_free_hsync) {
            if (cpc->monitor_free_hsync != cpc->monitor_min_hsync)
                cpc->monitor_free_hsync--;
        } else if (temp > cpc->monitor_free_hsync) {
            if (cpc->monitor_free_hsync != cpc->monitor_max_hsync)
                cpc->monitor_free_hsync++;
        }

        temp = cpc->monitor_hs_peak_to_end - cpc->monitor_hs_end_to_peak;
        if (temp < 0) {
            temp = -temp;
            if (temp > cpc->monitor_hs_start_pos)
                temp = cpc->monitor_hs_start_pos;
            temp >>= 3;
            if (!temp) temp++;
            if (temp > MONITOR_SYNC_INC_MAX)
                temp = MONITOR_SYNC_INC_MAX;
            cpc->monitor_hsync = cpc->monitor_free_hsync + temp;
        } else {
            if (temp > cpc->monitor_hs_start_pos)
                temp = cpc->monitor_hs_start_pos;
            temp >>= 3;
            if (!temp) temp++;
            if (temp > MONITOR_SYNC_DEC_MAX)
                temp = MONITOR_SYNC_DEC_MAX;
            cpc->monitor_hsync = cpc->monitor_free_hsync - temp;
        }
    } else {
        temp = cpc->monitor_hs_start_to_peak - cpc->monitor_hs_peak_to_end;
        if (!temp) {
            cpc->monitor_hsync = cpc->monitor_free_hsync;
        } else if (temp < 0) {
            temp = -temp;
            if (temp > cpc->monitor_hs_start_pos)
                temp = cpc->monitor_hs_start_pos;
            temp >>= 3;
            if (!temp) temp++;
            if (temp > MONITOR_SYNC_INC_MAX)
                temp = MONITOR_SYNC_INC_MAX;
            cpc->monitor_hsync = cpc->monitor_free_hsync + temp;
        } else {
            if (temp > cpc->monitor_hs_start_pos)
                temp = cpc->monitor_hs_start_pos;
            temp >>= 3;
            if (!temp) temp++;
            if (temp > MONITOR_SYNC_DEC_MAX)
                temp = MONITOR_SYNC_DEC_MAX;
            cpc->monitor_hsync = cpc->monitor_free_hsync - temp;
        }
    }

    cpc->monitor_hsync = clamp_int(cpc->monitor_hsync,
                                   cpc->monitor_min_hsync,
                                   cpc->monitor_max_hsync);
    cpc->monitor_hs_end_pos = 0;
}

static void cpc_monitor_advance(CPC *cpc) {
    cpc->monitor_hs_start_pos += 0x100;
    cpc->monitor_hs_end_pos += 0x100;
    cpc->monitor_hs_peak_pos += 0x100;
    cpc->monitor_hpos += 0x100;

    if (cpc->monitor_hpos >= cpc->monitor_hsync) {
        cpc->monitor_had_peak = true;
        cpc->monitor_hs_peak_pos = cpc->monitor_hpos - cpc->monitor_hsync;
        cpc->monitor_hs_start_to_peak =
            cpc->monitor_hs_start_pos - cpc->monitor_hs_peak_pos;
        cpc->monitor_hs_end_to_peak =
            cpc->monitor_hs_end_pos - cpc->monitor_hs_peak_pos;
        cpc->monitor_hpos =
            cpc->monitor_hs_peak_pos - cpc->monitor_hsync_duration;
        cpc->raster_y++;
        cpc->monitor_vline++;
    }
}

static bool cpc_monitor_finish_frame(CPC *cpc, bool crtc_vsync) {
    if ((!crtc_vsync || cpc->monitor_vline <= MONITOR_MIN_VSYNC) &&
        cpc->monitor_vline < MONITOR_MAX_VSYNC)
        return false;

    /* Match the vertical hold used by Caprice32. A standard 312-line frame
     * restarts drawing at -31, which centres the CPC image while allowing
     * early/short CRTC VSYNC tricks to pass without rolling the monitor. */
    cpc->raster_y = -(((cpc->monitor_vline - MONITOR_MIN_VHOLD) + 1) >> 1);
    cpc->monitor_vline = 0;
    cpc->monitor_frame_completed = true;
    return true;
}

static inline int cpc_monitor_px(const CPC *cpc) {
    return (cpc->monitor_hpos / 16) + MONITOR_X_ADJUST_PIXELS;
}

/* Advance ALL per-cycle peripheral state by `cycles` CPU T-states.
 * Covers: tape, audio sampling, CRTC/GA/render. Mirrors konCePCja's
 * z80_wait_states macro which advances CRTC + PSG + FDC + tape in
 * one bundle. 1984 doesn't model per-cycle PSG/FDC (PSG renders
 * once per frame in cpc_frame; FDC is event-driven, not per-cycle).
 *
 * Callable both from the per-instruction tail of cpc_frame() AND from
 * the Z80Bus::tick hook mid-instruction (for konCePCja-style IO split
 * timing). The pre-tick CRTC snapshot used by the render block lives
 * on the CPC struct (crtc_pre_ma/ra/de) so partial calls share state. */
static void cpc_advance_bus(CPC *cpc, int cycles) {
    if (cycles <= 0) return;
    /* Tape (no-op when no tape loaded / motor off) */
    tape_step(&cpc->tape, cycles);
    ppi_set_tape_level(&cpc->ppi, tape_level(&cpc->tape));
    /* Audio sampling at 44.1 kHz from the cassette signal */
    cpc->tape_audio_cycles += cycles * AUDIO_SAMPLE_RATE;
    while (cpc->tape_audio_cycles >= cpc->cpu_clk_hz &&
           cpc->tape_audio_pos < AUDIO_SAMPLES_FRAME) {
        cpc->tape_audio[cpc->tape_audio_pos++] =
            (tape_level(&cpc->tape) & 0x80) ? 2500 : -2500;
        cpc->tape_audio_cycles -= cpc->cpu_clk_hz;
    }
    cpc->crtc_cycle_acc += cycles;
    while (cpc->crtc_cycle_acc >= 4) {
        cpc->crtc_cycle_acc -= 4;
        crtc_tick(&cpc->crtc);

        bool new_hsync = crtc_hsync(&cpc->crtc);
        bool new_vsync = crtc_vsync(&cpc->crtc);
        bool new_crtc_line = crtc_new_scanline(&cpc->crtc);
        bool latch_mode = crtc_mode_latch(&cpc->crtc);

        /* GA interrupt counter on hsync falling edge (matches Caprice32) */
        if (!new_hsync && cpc->prev_hsync)
            ga_hsync(&cpc->ga);

        ppi_set_vsync(&cpc->ppi, new_vsync);

        /* The Gate Array and PPI observe raw CRTC VSYNC. The monitor has its
         * own vertical hold and decides separately whether that pulse ends
         * the displayed frame. */
        if (new_vsync && !cpc->prev_vsync) {
            ga_vsync_start(&cpc->ga);
        }

        /* --- Render 16 pixels for this char clock --- */
        int px = cpc_monitor_px(cpc);
        int py = cpc->raster_y;

        if (py >= 0 && py < CPC_SCREEN_H && px >= 0 && px <= CPC_SCREEN_W - 16) {
            u32 *row = cpc->display.pixels + py * CPC_SCREEN_W;
            if (cpc->crtc_pre_de) {
                /* Video address: bank=MA[13:12], raster=RA[2:0], col=MA[9:0] */
                u16 bank = (cpc->crtc_pre_ma >> 12) & 3;
                u16 col  = cpc->crtc_pre_ma & 0x3FF;
                u16 addr = (u16)((bank << 14) | ((cpc->crtc_pre_ra & 7) << 11) | (col << 1));
                u8  b0   = mem_read_video(&cpc->mem, addr);
                u8  b1   = mem_read_video(&cpc->mem, (u16)(addr + 1));
                render_char(row, px, &cpc->ga, b0, b1);
            } else {
                u32 c = cpc->ga.resolved_ink[16]; /* border */
                u32 *out = row + px;
                out[0]=c;  out[1]=c;  out[2]=c;  out[3]=c;
                out[4]=c;  out[5]=c;  out[6]=c;  out[7]=c;
                out[8]=c;  out[9]=c;  out[10]=c; out[11]=c;
                out[12]=c; out[13]=c; out[14]=c; out[15]=c;
            }
        }

        /* CRTC timing changes happen after the current character is output,
         * so the newly latched mode is visible from the next character. */
        if (latch_mode)
            ga_latch_mode(&cpc->ga);

        cpc_monitor_advance(cpc);
        cpc->raster_x = cpc->monitor_hpos / 256;

        /* The physical monitor free-runs through long/short CRTC lines, but
         * vertical hold is sampled when the CRTC starts a real scanline.
         * An 8-bit HCC wrap after a missed R0 comparator is not a newscan. */
        if (new_crtc_line && cpc_monitor_finish_frame(cpc, new_vsync))
            display_upload(&cpc->display);

        /* On a CPC the Gate Array only observes the first seven CRTC HSYNC
         * character clocks. The monitor-visible pulse starts a little later
         * and is also cut off there; narrow HSW values therefore do not yank
         * the display beam around. */
        if (new_hsync && cpc->crtc.hsc == 3)
            cpc_monitor_start_hsync(cpc);
        if (latch_mode)
            cpc_monitor_end_hsync(cpc);

        cpc->prev_hsync = new_hsync;
        cpc->prev_vsync = new_vsync;

        /* Capture next char's pre-tick state */
        cpc->crtc_pre_ma = cpc->crtc.ma;
        cpc->crtc_pre_ra = cpc->crtc.vlc;
        cpc->crtc_pre_de = cpc->crtc.display_enable;
    }
}

/* Z80Bus::tick callback: advance CRTC by `cycles` mid-instruction and
 * remember how many cycles we already accounted for so cpc_frame()
 * doesn't double-tick. Wired in cpc_init(). */
static void cpc_bus_tick(void *ctx, int cycles) {
    CPC *cpc = (CPC *)ctx;
    if (cycles <= 0) return;
    cpc_advance_bus(cpc, cycles);
    cpc->bus_ticked_in_step += cycles;
}

void cpc_frame(CPC *cpc) {
    if (cpc->paused && !cpc->step_once) return;
    bool was_stepping = cpc->step_once;
    cpc->step_once = false;

    int target = cpc->cycles_per_frame + cpc->cycle_debt;
    if (target <= 0)
        target = cpc->cycles_per_frame;
    int max_target = target + cpc->cycles_per_frame;
    int done   = 0;
    bool stop_early = false;
    bool frame_done = false;
    cpc->monitor_frame_completed = false;

    /* Master gate: when no trace env is set, the entire per-instruction
     * debug block is short-circuited. Branch is predicted-taken to
     * "off" in production runs (no env vars set). */
    trace_check_master();
    /* The frontend presents once after cpc_frame(). If a CRTC frame runs
     * slightly past the nominal budget, returning before display_upload()
     * leaves the host presenting stale renderer contents, visible as rapid
     * flashing. Run a bounded overrun until one monitor frame completed. */
    while ((done < target || !frame_done) && done < max_target) {
        /* #102 wedge instrumentation: at every firmware-IRQ-handler
         * 'CALL $BDF4' site ($00D9 in OS ROM), capture what's actually
         * at $BDF4 and the value of the secondary-tick counter $B8BF.
         * Hypothesis: the panic ($BE54 spin) fires because the CP/M+
         * kernel image's placeholder JP $BE4F at $BDF4 has not yet
         * been replaced by the kernel's real handler when the
         * firmware's 8.3 Hz secondary tick hits zero.
         *
         * Gate on ONE_K_TRACE_BDF4 (set once at startup); only print
         * the first 30 hits and any hit where $BDF4 holds the panic
         * stub (C3 4F BE) so we can correlate failing boots. */
        static int trace_bdf4 = -1;
        if (g_trace_any && trace_bdf4 == -1) trace_bdf4 = dbg_getenv("ONE_K_TRACE_BDF4") ? 1 : 0;
        if (g_trace_any && trace_bdf4 && cpc->cpu.pc == 0x00D9 && cpc->mem.lower_rom_enabled) {
            static int hits = 0;
            u8 a = mem_read(&cpc->mem, 0xBDF4);
            u8 b = mem_read(&cpc->mem, 0xBDF5);
            u8 c = mem_read(&cpc->mem, 0xBDF6);
            u8 be4f = mem_read(&cpc->mem, 0xBE4F);
            u8 be55 = mem_read(&cpc->mem, 0xBE55);
            u8 be56 = mem_read(&cpc->mem, 0xBE56);
            u8 b8bf = mem_read(&cpc->mem, 0xB8BF);
            /* Real "self-loop / halt" = $BE54 holds 'JP $BE4F'. The
             * "$BDF4 = C3 4F BE" is just the kernel's normal hook to
             * $BE4F — safe iff $BE4F's body chains to firmware. */
            bool halt_loop = (be55 == 0x4F && be56 == 0xBE);
            if (hits < 30 || halt_loop)
                fprintf(stderr, "[BDF4] hit#%-4d frame=%d  $BDF4=%02X %02X %02X  $BE4F=%02X  $BE55=%02X $BE56=%02X  ram_cfg=%02X  $B8BF=%02X%s\n",
                        hits, cpc_frame_count, a, b, c, be4f, be55, be56,
                        cpc->mem.ram_bank, b8bf,
                        halt_loop ? "  <<< HALT LOOP at $BE54!" : "");
            hits++;
        }
        /* Capture CRTC state BEFORE tick for this character clock. Stored
         * on the struct (not stack-local) so the Z80 bus tick hook can
         * run a partial CRTC advance in the middle of an instruction and
         * the resumed chunk picks up where the previous one left off. */
        cpc->crtc_pre_ma = cpc->crtc.ma;
        cpc->crtc_pre_ra = cpc->crtc.vlc;
        cpc->crtc_pre_de = cpc->crtc.display_enable;

        /* Reset-PC ring fill, gated on the global trace flag — needs to
         * record every Z80 instruction, not just the panic-trace one,
         * so that on a kernel reboot we can see the last 8192 PCs.
         * Trigger the dump the FIRST time PC enters $FF00-$FFFF when
         * we were previously in the kernel — that's the runaway entry,
         * BEFORE banking gets reset by firmware. Capture a snapshot
         * here so we can disassemble the pre-corruption memory state. */
        if (g_trace_any && dbg_getenv("ONE_K_TRACE_BDF4")) {
            g_reset_pc_ring[g_reset_pc_idx & 0x1FFF] = cpc->cpu.pc;
            g_reset_sp_ring[g_reset_pc_idx & 0x1FFF] = cpc->cpu.sp;
            g_reset_bank_ring[g_reset_pc_idx & 0x1FFF] = cpc->mem.ram_bank;
            g_reset_pc_idx++;
            static int runaway_dumped = 0;
            static u16 last_pc_for_runaway = 0;
            bool transition_into_ffxx =
                (cpc->cpu.pc >= 0xFF00 && cpc->cpu.pc <= 0xFFFF) &&
                !(last_pc_for_runaway >= 0xFF00 && last_pc_for_runaway <= 0xFFFF);
            last_pc_for_runaway = cpc->cpu.pc;
            if (!runaway_dumped && transition_into_ffxx
                    && !cpc->mem.lower_rom_enabled
                    && cpc_frame_count > 300
                    && g_reset_pc_idx > 8192) {
                runaway_dumped = 1;
                fprintf(stderr, "[RUNAWAY] frame=%d  first entry into "
                                "$FF00-$FFFF at PC=%04X SP=%04X — dumping ring + sna\n",
                        cpc_frame_count, cpc->cpu.pc, cpc->cpu.sp);
                const char *p = dbg_getenv("ONE_K_RESET_SNA");
                if (p && *p) snapshot_save(cpc, p);
                u32 start = g_reset_pc_idx & 0x1FFF;
                u16 last = 0xFFFF; int run = 0;
                for (int i = 0; i < 8192; i++) {
                    u32 j = (start + i) & 0x1FFF;
                    u16 pc = g_reset_pc_ring[j];
                    if (pc == last) { run++; continue; }
                    if (run > 0) fprintf(stderr, "        (x%d more)\n", run);
                    run = 0; last = pc;
                    fprintf(stderr, "  PC=%04X  SP=%04X  bank=%02X\n", pc, g_reset_sp_ring[j], g_reset_bank_ring[j]);
                }
                fflush(stderr);
            }
            static int reset_dumped = 0;
            if (!reset_dumped && cpc->cpu.pc == 0x0000
                    && cpc->mem.lower_rom_enabled
                    && g_reset_pc_idx > 8192) {
                reset_dumped = 1;
                fprintf(stderr, "[RESET] frame=%d  PC=$0000 with lrom=1 — "
                                "kernel rebooted. Last 8192 PCs leading up:\n",
                        cpc_frame_count);
                u32 start = g_reset_pc_idx & 0x1FFF;
                u16 last = 0xFFFF; int run = 0;
                for (int i = 0; i < 8192; i++) {
                    u32 j = (start + i) & 0x1FFF;
                    u16 pc = g_reset_pc_ring[j];
                    if (pc == last) { run++; continue; }
                    if (run > 0) fprintf(stderr, "        (x%d more)\n", run);
                    run = 0; last = pc;
                    fprintf(stderr, "  PC=%04X  SP=%04X  bank=%02X\n", pc, g_reset_sp_ring[j], g_reset_bank_ring[j]);
                }
                fflush(stderr);
            }
        }
        /* Lightweight panic-trace: fires the first time PC hits the
         * address in ONE_K_TRACE_PANIC (hex), then dumps a small ring
         * buffer of preceding PCs/SPs so we can see the call chain
         * that led into the CP/M+ kernel halt loop at $BE4F-$BE54.
         * Independent of the heavier ONE_K_TRACE_CRASH below. */
        {
            #define PANIC_RING 32768
            static int panic_pc = -1;
            static u16 ring_pc[PANIC_RING], ring_sp[PANIC_RING], ring_bc[PANIC_RING], ring_hl[PANIC_RING];
            static u32 ring_idx = 0;
            static int panic_dumped = 0;
            if (panic_pc == -1) {
                const char *e = dbg_getenv("ONE_K_TRACE_PANIC");
                panic_pc = (e && *e) ? (int)strtoul(e, NULL, 16) : 0;
            }
            if (panic_pc > 0) {
                ring_pc[ring_idx & (PANIC_RING-1)] = cpc->cpu.pc;
                ring_sp[ring_idx & (PANIC_RING-1)] = cpc->cpu.sp;
                ring_bc[ring_idx & (PANIC_RING-1)] = cpc->cpu.bc;
                ring_hl[ring_idx & (PANIC_RING-1)] = cpc->cpu.hl;
                ring_idx++;
                if (!panic_dumped && cpc->cpu.pc == (u16)panic_pc) {
                    panic_dumped = 1;
                    fprintf(stderr, "[PANIC] PC reached %04X — last PANIC_RING PCs (oldest first):\n",
                            (unsigned)panic_pc);
                    u32 start = ring_idx & (PANIC_RING-1);
                    u16 last = 0xFFFF; int run = 0;
                    for (int i = 0; i < PANIC_RING; i++) {
                        u32 j = (start + i) & (PANIC_RING-1);
                        u16 pc = ring_pc[j];
                        if (pc == last) { run++; continue; }
                        if (run > 0) fprintf(stderr, "        (x%d more)\n", run);
                        run = 0; last = pc;
                        fprintf(stderr, "  PC=%04X SP=%04X BC=%04X HL=%04X\n",
                                pc, ring_sp[j], ring_bc[j], ring_hl[j]);
                    }
                    fflush(stderr);
                }
            }
        }
        if (dbg_getenv("ONE_K_TRACE_CRASH")) {
            /* Wider ring buffer + capture SP, BC, HL too */
            static u16 pcs[8192], sps[8192], bcs[8192], hls[8192];
            static u32 idx = 0;
            static int dumped = 0;
            static int zero_visits = 0;
            static int prev_was_nonzero = 0;
            extern u32 ide_cmd_count_for_crash_trace;
            /* Only arm the trigger AFTER divergence point (IDE cmd > 200) */
            int armed = ide_cmd_count_for_crash_trace > 200;
            static int armed_zero_visits = 0;
            pcs[idx & 8191] = cpc->cpu.pc;
            sps[idx & 8191] = cpc->cpu.sp;
            bcs[idx & 8191] = cpc->cpu.bc;
            /* Pack lower_rom + upper_rom_select into hls slot for compactness */
            hls[idx & 8191] = (u16)((cpc->mem.lower_rom_enabled ? 0x8000 : 0) |
                                    (cpc->mem.upper_rom_enabled ? 0x4000 : 0) |
                                    (cpc->mem.upper_rom_select));
            idx++;
            /* True firmware reset: PC=0 with lower ROM enabled.
             * CP/M+ uses PC=0 for warm boot but with lower ROM DISABLED. */
            int real_reset = (cpc->cpu.pc == 0x0000) && cpc->mem.lower_rom_enabled;
            if (real_reset) {
                if (prev_was_nonzero) {
                    zero_visits++;
                    if (armed) armed_zero_visits++;
                }
                prev_was_nonzero = 0;
            } else {
                prev_was_nonzero = 1;
            }
            if (!dumped && armed_zero_visits >= 1) {
                dumped = 1;
                fprintf(stderr, "[CRASH] reset (PC=0 visit #2) — full ring buffer (newest last):\n");
                /* Walk forward from oldest, suppressing runs of same PC */
                u32 start = idx & 8191;
                u32 last_pc = 0xFFFF;
                int run = 0;
                int shown = 0;
                for (int i = 0; i < 8192; i++) {
                    u32 j = (start + i) & 8191;
                    u16 pc = pcs[j];
                    if (pc == 0 && i < 1000) continue;  /* skip leading zeros (unfilled) */
                    if (pc == last_pc) { run++; continue; }
                    if (run > 0) fprintf(stderr, "        (x%d more)\n", run);
                    run = 0;
                    last_pc = pc;
                    {
                        u16 r = hls[j];
                        fprintf(stderr, "  PC=%04X SP=%04X BC=%04X %s%s rom=%02X\n",
                                pc, sps[j], bcs[j],
                                (r & 0x8000) ? "LROM " : "lram ",
                                (r & 0x4000) ? "UROM " : "uram ",
                                (u8)(r & 0xFF));
                    }
                    if (++shown > 5000) { fprintf(stderr, "  ... (truncated)\n"); break; }
                }
            }
        }

        if (dbg_getenv("ONE_K_TRACE_LDIR_BANK")) {
            static int hit = 0;
            if (!hit && cpc->cpu.pc == 0xC636 && cpc->cpu.de == 0x7600
                && cpc->mem.upper_rom_enabled && cpc->mem.upper_rom_select == 0x01) {
                hit = 1;
                fprintf(stderr, "[LDIR2 START] DE=7600 BC=%04X HL=%04X ram_bank=%02X upper_rom_enabled=%d lower_rom_enabled=%d upper_rom_select=%02X\n",
                        cpc->cpu.bc, cpc->cpu.hl, cpc->mem.ram_bank,
                        cpc->mem.upper_rom_enabled, cpc->mem.lower_rom_enabled, cpc->mem.upper_rom_select);
            }
            static int hit_end = 0;
            if (!hit_end && cpc->cpu.pc == 0xC638 && cpc->cpu.de >= 0x79F0
                && cpc->mem.upper_rom_enabled && cpc->mem.upper_rom_select == 0x01) {
                hit_end = 1;
                fprintf(stderr, "[LDIR2 END] DE=%04X BC=%04X HL=%04X ram_bank=%02X RAM[0x7600..7607]=%02X %02X %02X %02X %02X %02X %02X %02X\n",
                        cpc->cpu.de, cpc->cpu.bc, cpc->cpu.hl, cpc->mem.ram_bank,
                        cpc->mem.ram[0x7600], cpc->mem.ram[0x7601], cpc->mem.ram[0x7602], cpc->mem.ram[0x7603],
                        cpc->mem.ram[0x7604], cpc->mem.ram[0x7605], cpc->mem.ram[0x7606], cpc->mem.ram[0x7607]);
            }
        }
        /* #102 outcome detector. Captures printed text from firmware
         * TXT_OUTPUT (PC=$BB5A; lower-ROM enabled) and CP/M+ BDOS
         * C_WRITE (PC=$0005 with C=2). Writes to a circular char buf
         * so a post-run scan can tell us whether the kernel reached
         * `A0:CPM>` (success), reached the CP/M+ banner only
         * (mid-boot stall), or stayed in BASIC (autostart paste race
         * — not a #102 fail). Plus a reboot signature: HDCPM ROM
         * activated AGAIN after BDOS was reached.
         *
         * Gated on ONE_K_CAP_TEXT=N where N is the boot index (just
         * an id passed through to stderr to disambiguate batch runs).
         */
        {
            static int cap_text = -1;
            if (cap_text == -1)
                cap_text = (g_debug_enabled && dbg_getenv("ONE_K_CAP_TEXT")) ? 1 : 0;
        if (cap_text) {
            static int boot_reached_bdos = 0;
            if (cpc->cpu.pc == 0xBB5A && cpc->mem.lower_rom_enabled) {
                u8 c = cpc->cpu.a;
                if (c >= 0x20 && c < 0x7F)
                    fputc(c, stderr);
                else if (c == '\r')
                    fputc('\n', stderr);
                /* sentinel: a `>` at this point right after `A0:CPM`
                 * means the prompt printed; we leave higher-level
                 * detection to the post-run grep. */
            }
            if (cpc->cpu.pc == 0x0005 && cpc->cpu.c == 2) {
                if (!boot_reached_bdos) {
                    boot_reached_bdos = 1;
                    fprintf(stderr, "\n[CAP] BDOS C_WRITE reached at frame=%d\n", cpc_frame_count);
                }
                u8 c = cpc->cpu.e;
                if (c >= 0x20 && c < 0x7F)
                    fputc(c, stderr);
                else if (c == '\r')
                    fputc('\n', stderr);
            }
            /* Reboot detector: HDCPM ROM init runs at PC=$C000 area
             * with upper_rom_select==1. If that happens AFTER we've
             * already reached BDOS, the kernel rebooted (= fail). */
            if (boot_reached_bdos
                && cpc->cpu.pc >= 0xC000 && cpc->cpu.pc < 0xC100
                && cpc->mem.upper_rom_enabled
                && cpc->mem.upper_rom_select == 0x01) {
                static int reported = 0;
                if (!reported) {
                    reported = 1;
                    fprintf(stderr, "\n[CAP] REBOOT detected: HDCPM ROM re-entered at frame=%d PC=%04X (post-BDOS)\n",
                            cpc_frame_count, cpc->cpu.pc);
                }
            }
            /* Earlier reboot detector: PC=0x0000 firmware-reset vector
             * AFTER the boot loader started executing (PC=0x0100+ was
             * reached). Catches crashes BEFORE BDOS is reached. */
            {
                static int boot_loader_reached = 0;
                static int early_reboot_reported = 0;
                if (cpc->cpu.pc >= 0x0100 && cpc->cpu.pc < 0x4000
                    && !cpc->mem.lower_rom_enabled)
                    boot_loader_reached = 1;
                if (boot_loader_reached && !early_reboot_reported
                    && cpc->cpu.pc == 0x0000
                    && cpc->mem.lower_rom_enabled) {
                    early_reboot_reported = 1;
                    fprintf(stderr, "\n[CAP] EARLY REBOOT: PC=0000 firmware reset at frame=%d (pre-BDOS)\n",
                            cpc_frame_count);
                }
            }
        }
        }

        /* TEMP #102: log every entry to the relocated LDIR patcher at
         * PC=0117/0119, plus first 8 PCs before entering, to find what
         * triggers the spurious second pass. */
        if (dbg_getenv("ONE_K_TRACE_LDIR")) {
            #define LDIR_RING_N 2048
            static u16 ring[LDIR_RING_N]; static int ringp = 0;
            static int hits_117 = 0;
            if (cpc->cpu.pc == 0x117 && !cpc->mem.lower_rom_enabled
                && cpc->cpu.hl == 0xBDF4) {
                hits_117++;
                /* Only dump the SECOND hit; condense by removing tight loops. */
                if (hits_117 == 2) {
                    fprintf(stderr, "[LDIR-2nd] frame=%d HL=BDF4\n", cpc_frame_count);
                    u16 last = 0xFFFF; int run = 0;
                    for (int i = 0; i < LDIR_RING_N; i++) {
                        u16 p = ring[(ringp + i) & (LDIR_RING_N-1)];
                        if (p == last) { run++; continue; }
                        if (run > 0) fprintf(stderr, " (x%d)", run);
                        run = 0; last = p;
                        if ((i & 7) == 0) fprintf(stderr, "\n ");
                        fprintf(stderr, " %04X", p);
                    }
                    if (run > 0) fprintf(stderr, " (x%d)", run);
                    fprintf(stderr, "\n");
                }
            }
            ring[ringp & (LDIR_RING_N-1)] = cpc->cpu.pc;
            ringp++;
        }
        /* The GA acknowledges an interrupt as the CPU accepts it, before
         * interrupt-entry cycles advance the scanline counter. */
        if (cpc->cpu.pending_irq && cpc->cpu.iff1 && !cpc->cpu.ei_delay)
            ga_irq_ack(&cpc->ga);
        int t = z80_step(&cpc->cpu, &cpc->bus);
        /* Firmware text-out hook for --kbd-pty. The CPC ROM exposes
         * "TXT WR CHAR" at &BB5A — A holds the byte to print. By
         * sampling AFTER the step we catch the moment the CPU has
         * just entered the vector (PC is at the second instruction
         * already, but A is still the caller's). We rely on the next
         * step to detect this; cheap, and only emits when a kbd PTY
         * is actually open. */
        if (cpc->cpu.pc == 0xBB5A && kbd_pty_is_open())
            kbd_pty_emit_char(cpc->cpu.a);
        done += t;
        g_total_t += t;
        /* Post-instruction bus advance — tape, audio sampling, CRTC,
         * GA interrupt counter, render. The bus->tick hook may have
         * already advanced part of this step from inside an IO opcode;
         * subtract that count so we don't double-tick. */
        int remaining = t - cpc->bus_ticked_in_step;
        if (remaining < 0) remaining = 0;
        cpc_advance_bus(cpc, remaining);
        frame_done = cpc->monitor_frame_completed;

        /* Deliver pending Gate Array interrupt. */
        if (cpc->ga.interrupt_pending) {
            cpc->ga.interrupt_pending = false;
            z80_interrupt(&cpc->cpu);
        }
        if (cpc->cpu.int_accepted) {
            cpc->cpu.int_accepted = false;
            if (dbg_getenv("ONE_K_TRACE_IRQ")) {
                extern long long g_total_t;
                static int irq_n = 0;
                static long long last_t = 0;
                u16 ret_pc = (u16)mem_read(&cpc->mem, cpc->cpu.sp)
                           | ((u16)mem_read(&cpc->mem, cpc->cpu.sp + 1) << 8);
                long long dt = g_total_t - last_t;
                last_t = g_total_t;
                fprintf(stderr, "[IRQ #%d] frame=%d ret_pc=%04X sl=%d dt=%lld\n",
                    ++irq_n, cpc_frame_count, ret_pc, cpc->ga.interrupt_counter, dt);
            }
        }

        /* Breakpoint check */
        for (int b = 0; b < CPC_MAX_BREAKPOINTS; b++) {
            if (cpc->bp_enabled[b] && cpc->cpu.pc == cpc->breakpoints[b]) {
                cpc->paused   = true;
                stop_early    = true;
                break;
            }
        }
        if (frame_done || stop_early || was_stepping) break;
    }

    cpc->cycle_debt = (frame_done || stop_early) ? 0 : (done - target);
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
        if (!SDL_PutAudioStreamData(cpc->audio_stream,
                                    audio_buf, (int)sizeof(audio_buf))) {
            fprintf(stderr, "SDL_PutAudioStreamData: %s\n", SDL_GetError());
        } else if (dbg_getenv("ONE_K_TRACE_AUDIO") &&
                   (cpc_frame_count % 50) == 0) {
            s16 min = audio_buf[0], max = audio_buf[0];
            for (int i = 1; i < AUDIO_SAMPLES_FRAME; i++) {
                if (audio_buf[i] < min) min = audio_buf[i];
                if (audio_buf[i] > max) max = audio_buf[i];
            }
            fprintf(stderr, "[audio f%d] samples=%d..%d queued=%d bytes\n",
                    cpc_frame_count, min, max,
                    SDL_GetAudioStreamQueued(cpc->audio_stream));
        }
    }
}

void cpc_key_event(CPC *cpc, SDL_Scancode sc, bool pressed) {
    kbd_sdl_key(&cpc->kbd, sc, pressed);
}

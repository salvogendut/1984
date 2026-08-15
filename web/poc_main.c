/* Minimal WASM proof-of-concept main for the 1984 core.
 *
 * Exposes a tiny C API to the browser glue:
 *   - poc_init():     boot a plain CPC 6128 from the embedded ROMs
 *   - poc_step():     run one emulated frame (monitor completes one frame)
 *   - poc_pixels():   pointer to the 768x272 framebuffer (u32 0x00RRGGBB)
 *   - poc_key():      SDL_Scancode key down/up
 *   - poc_load_disk(): mount a .dsk into drive A or B from the virtual FS
 *   - poc_*_snapshot(): load or save an SNA through the virtual FS
 *   - poc_autorun():  queue RUN"file" after a frame-counted boot delay
 *   - poc_mouse_*():  browser pointer input through the AMX adapter
 *   - poc_audio_*():  ring-buffer access for the PSG audio (interleaved s16)
 *
 * No SDL runtime is used — the browser reads the framebuffer and the audio
 * ring buffer directly from wasm memory; the SDL3 headers are only pulled in
 * for the type definitions (Display holds SDL pointers, kbd_sdl_key takes
 * SDL_Scancode).
 */
#include <emscripten.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "cpc.h"
#include "kbd.h"
#include "disk.h"
#include "mem.h"
#include "paste.h"
#include "snapshot.h"
#include "symbols.h"
#include "z80dis.h"

static CPC g_cpc;
static Paste g_paste;
static int g_autorun_frames;
static char g_autorun_command[256];

/* ---- browser machine-language monitor ---- */
#define POC_DEBUG_WATCH_MAX 16
#define POC_DEBUG_EVENT_MAX 64
#define POC_DEBUG_DIS_LINES 12

typedef void (*PocCoreMemWrite)(void *, u16, u8);

typedef struct {
    unsigned int serial;
    u16 addr;
    u16 pc;
    u8 old_value;
    u8 new_value;
    u8 watch_slot;
} PocDebugWriteEvent;

enum {
    POC_DEBUG_RUNNING = 0,
    POC_DEBUG_PAUSE,
    POC_DEBUG_BREAKPOINT,
    POC_DEBUG_STEP_IN,
    POC_DEBUG_STEP_BACK,
    POC_DEBUG_STEP_OUT,
    POC_DEBUG_NEXT
};

static PocCoreMemWrite g_core_mem_write;
static bool g_debug_watch_enabled[POC_DEBUG_WATCH_MAX];
static u16 g_debug_watch_addr[POC_DEBUG_WATCH_MAX];
static PocDebugWriteEvent g_debug_events[POC_DEBUG_EVENT_MAX];
static unsigned int g_debug_event_serial;
static CPC g_debug_checkpoint;
static bool g_debug_checkpoint_valid;
static int g_debug_stop_reason;
static CpcBreakpointId g_debug_temp_bp_id = CPC_BREAKPOINT_INVALID_ID;
static u16 g_debug_temp_bp_addr;
static int g_debug_temp_bp_reason;
static int g_debug_step_reason;
static u8 g_debug_dis_memory[0x10000];
static char g_debug_disassembly[2048];

static void poc_debug_reset_state(void) {
    memset(g_debug_watch_enabled, 0, sizeof(g_debug_watch_enabled));
    memset(g_debug_watch_addr, 0, sizeof(g_debug_watch_addr));
    memset(g_debug_events, 0, sizeof(g_debug_events));
    g_debug_event_serial = 0;
    g_debug_checkpoint_valid = false;
    g_debug_stop_reason = POC_DEBUG_RUNNING;
    g_debug_temp_bp_id = CPC_BREAKPOINT_INVALID_ID;
    g_debug_temp_bp_addr = 0;
    g_debug_temp_bp_reason = POC_DEBUG_RUNNING;
    g_debug_step_reason = POC_DEBUG_STEP_IN;
}

static void poc_debug_mem_write(void *ctx, u16 addr, u8 value) {
    CPC *cpc = ctx;
    u8 old_value = mem_read(&cpc->mem, addr);
    if (g_core_mem_write)
        g_core_mem_write(ctx, addr, value);
    u8 new_value = mem_read(&cpc->mem, addr);

    for (int slot = 0; slot < POC_DEBUG_WATCH_MAX; slot++) {
        if (!g_debug_watch_enabled[slot] || g_debug_watch_addr[slot] != addr)
            continue;
        unsigned int serial = ++g_debug_event_serial;
        PocDebugWriteEvent *event =
            &g_debug_events[serial % POC_DEBUG_EVENT_MAX];
        event->serial = serial;
        event->addr = addr;
        event->pc = cpc->cpu.pc;
        event->old_value = old_value;
        event->new_value = new_value;
        event->watch_slot = (u8)slot;
    }
}

static void poc_debug_install_mem_hook(void) {
    g_core_mem_write = g_cpc.bus.mem_write;
    g_cpc.bus.mem_write = poc_debug_mem_write;
}

static void poc_cancel_paste(void) {
    paste_free(&g_paste);
    paste_init(&g_paste);
    g_autorun_frames = 0;
    g_autorun_command[0] = '\0';
}

/* ---- audio ring buffer (interleaved stereo s16, 2 seconds @ 44.1 kHz) ---- */
#define AUDIO_RING_SAMPLES (44100 * 4)
static s16 g_audio_ring[AUDIO_RING_SAMPLES];
static int g_audio_w = 0;   /* write index (producer = emulator) */
static int g_audio_r = 0;   /* read index  (consumer = browser)  */

static void poc_audio_sink(void *userdata, const s16 *samples, int frames,
                           int sample_rate) {
    (void)userdata; (void)sample_rate;
    int n = frames * 2;
    for (int i = 0; i < n; i++) {
        int w = (g_audio_w + 1) % AUDIO_RING_SAMPLES;
        if (w == g_audio_r) break;            /* full — drop this frame */
        g_audio_ring[g_audio_w] = samples[i];
        g_audio_w = w;
    }
}

EMSCRIPTEN_KEEPALIVE void poc_audio_reset(void) { g_audio_w = 0; g_audio_r = 0; }
EMSCRIPTEN_KEEPALIVE int  poc_audio_avail(void) {
    return (g_audio_w - g_audio_r + AUDIO_RING_SAMPLES) % AUDIO_RING_SAMPLES;
}
EMSCRIPTEN_KEEPALIVE int  poc_audio_read_pos(void) { return g_audio_r; }
EMSCRIPTEN_KEEPALIVE short *poc_audio_buffer(void) { return g_audio_ring; }
EMSCRIPTEN_KEEPALIVE void poc_audio_advance(int n) {
    g_audio_r = (g_audio_r + n) % AUDIO_RING_SAMPLES;
}

/* ---- emulator lifecycle ----
 * model 0 = CPC 6128 (OS/BASIC/AMSDOS), 1 = CPC 6128 Plus (cartridge;
 * defaults to the bundled system cartridge when cartridge is NULL).
 * May be called repeatedly to switch machines. */

static bool poc_memory_size_supported(int memory_kb) {
    return memory_kb == 128 || memory_kb == 256 ||
           memory_kb == 512 || memory_kb == 1024;
}

EMSCRIPTEN_KEEPALIVE int poc_init_model(int model, const char *cartridge) {
    int rc;
    int memory_kb = g_cpc.mem.ram_size / 1024;
    if (!poc_memory_size_supported(memory_kb))
        memory_kb = 128;
    poc_cancel_paste();
    tape_eject(&g_cpc.tape);
    remu_debug_clear(&g_cpc.remu_debug);
    cpc_breakpoints_destroy(&g_cpc);
    if (model == 1) {
        rc = cpc_init(&g_cpc, MODEL_6128_PLUS, NULL, NULL,
                      cartridge ? cartridge : "roms/system.cpr", 1);
    } else {
        rc = cpc_init(&g_cpc, MODEL_6128, "roms/OS_6128.ROM",
                      "roms/BASIC_1.1.ROM", NULL, 1);
        if (rc == 0)
            mem_load_amsdos(&g_cpc.mem, "roms/AMSDOS.ROM");
    }
    if (rc != 0)
        return -1;
    g_cpc.mem.ram_size = memory_kb * 1024;
    cpc_set_audio_sink(&g_cpc, poc_audio_sink, NULL);
    poc_debug_reset_state();
    poc_debug_install_mem_hook();
    return 0;
}

EMSCRIPTEN_KEEPALIVE int poc_init(void) { return poc_init_model(0, NULL); }

EMSCRIPTEN_KEEPALIVE int poc_load_cartridge(const char *path) {
    return poc_init_model(1, path);
}

/* Warm reset of the current machine (keeps loaded ROMs/cartridge). */
EMSCRIPTEN_KEEPALIVE void poc_reset(void) {
    poc_cancel_paste();
    cpc_reset(&g_cpc);
    g_cpc.paused = false;
    g_cpc.step_once = false;
    g_debug_checkpoint_valid = false;
    g_debug_stop_reason = POC_DEBUG_RUNNING;
    if (g_debug_temp_bp_id != CPC_BREAKPOINT_INVALID_ID)
        cpc_breakpoint_clear(&g_cpc, g_debug_temp_bp_id);
    g_debug_temp_bp_id = CPC_BREAKPOINT_INVALID_ID;
    g_debug_temp_bp_reason = POC_DEBUG_RUNNING;
}

/* ---- M4 expansion board ---- */

EMSCRIPTEN_KEEPALIVE int poc_set_m4(int enabled) {
    if (!enabled) {
        g_cpc.m4 = false;
        mem_unload_rom_ext(&g_cpc.mem, M4_ROM_SLOT);
        return 0;
    }
    if (mem_load_rom_ext(&g_cpc.mem, M4_ROM_SLOT, "roms/M4ROM.ROM") != 0)
        return -1;
    g_cpc.mx4 = true;
    g_cpc.m4 = true;
    memcpy(g_cpc.m4_card.cfg_mem,
           &g_cpc.mem.rom_ext[M4_ROM_SLOT][0xF400 - 0xC000],
           sizeof(g_cpc.m4_card.cfg_mem));
    m4_install_helper_shim(&g_cpc.m4_card, &g_cpc.mem);
    return 0;
}

EMSCRIPTEN_KEEPALIVE int poc_m4_enabled(void) {
    return g_cpc.m4 ? 1 : 0;
}

/* Mount a raw FAT SD image already written to the virtual filesystem. The
 * image bytes are held in browser memory (MEMFS); guest writes are kept in
 * that file until the JS downloads it on eject. */
EMSCRIPTEN_KEEPALIVE int poc_mount_m4_sd(const char *path) {
    if (!path || !path[0])
        return -1;
    m4_set_image(&g_cpc.m4_card, path);
    return g_cpc.m4_card.image_mounted ? 0 : -1;
}

EMSCRIPTEN_KEEPALIVE void poc_eject_m4_sd(void) {
    m4_set_image(&g_cpc.m4_card, "");
}

/* Select an expansion size and cold-start RAM without replacing the current
 * ROM/cartridge or mounted media. */
EMSCRIPTEN_KEEPALIVE int poc_set_memory_kb(int memory_kb) {
    if (!poc_memory_size_supported(memory_kb))
        return -1;
    memset(g_cpc.mem.ram, 0, sizeof(g_cpc.mem.ram));
    g_cpc.mem.ram_size = memory_kb * 1024;
    poc_reset();
    poc_audio_reset();
    return 0;
}

EMSCRIPTEN_KEEPALIVE int poc_memory_kb(void) {
    return g_cpc.mem.ram_size / 1024;
}

EMSCRIPTEN_KEEPALIVE int poc_load_snapshot(const char *path) {
    if (!path || !path[0])
        return -1;
    poc_cancel_paste();
    if (g_debug_temp_bp_id != CPC_BREAKPOINT_INVALID_ID)
        cpc_breakpoint_clear(&g_cpc, g_debug_temp_bp_id);
    g_debug_temp_bp_id = CPC_BREAKPOINT_INVALID_ID;
    int rc = snapshot_load(&g_cpc, path);
    if (rc != 0)
        return rc;

    g_debug_temp_bp_reason = POC_DEBUG_RUNNING;
    g_debug_checkpoint_valid = false;
    g_debug_stop_reason = POC_DEBUG_RUNNING;
    g_cpc.paused = false;
    g_cpc.step_once = false;
    poc_audio_reset();
    return 0;
}

EMSCRIPTEN_KEEPALIVE int poc_save_snapshot(const char *path) {
    if (!path || !path[0])
        return -1;
    return snapshot_save(&g_cpc, path);
}

EMSCRIPTEN_KEEPALIVE int poc_step(void) {
    if (g_autorun_frames > 0 && --g_autorun_frames == 0)
        paste_text(&g_paste, g_autorun_command);
    paste_tick(&g_paste, &g_cpc.kbd);
    bool stepping = g_cpc.step_once;
    int cycles = cpc_frame(&g_cpc);
    if (g_cpc.paused) {
        if (g_debug_temp_bp_id != CPC_BREAKPOINT_INVALID_ID &&
                g_cpc.cpu.pc == g_debug_temp_bp_addr) {
            cpc_breakpoint_clear(&g_cpc, g_debug_temp_bp_id);
            g_debug_temp_bp_id = CPC_BREAKPOINT_INVALID_ID;
            g_debug_stop_reason = g_debug_temp_bp_reason;
            g_debug_temp_bp_reason = POC_DEBUG_RUNNING;
        } else {
            bool user_breakpoint = cpc_breakpoint_match(
                &g_cpc, g_cpc.cpu.pc, g_debug_temp_bp_id) !=
                CPC_BREAKPOINT_INVALID_ID;
            if (user_breakpoint)
                g_debug_stop_reason = POC_DEBUG_BREAKPOINT;
            else if (stepping)
                g_debug_stop_reason = g_debug_step_reason;
            else if (g_debug_stop_reason == POC_DEBUG_RUNNING)
                g_debug_stop_reason = POC_DEBUG_BREAKPOINT;
        }
    }
    return cycles;
}

EMSCRIPTEN_KEEPALIVE void poc_debug_pause(void) {
    if (g_debug_temp_bp_id != CPC_BREAKPOINT_INVALID_ID) {
        cpc_breakpoint_clear(&g_cpc, g_debug_temp_bp_id);
        g_debug_temp_bp_id = CPC_BREAKPOINT_INVALID_ID;
        g_debug_temp_bp_reason = POC_DEBUG_RUNNING;
    }
    g_cpc.paused = true;
    g_cpc.step_once = false;
    g_debug_stop_reason = POC_DEBUG_PAUSE;
}

EMSCRIPTEN_KEEPALIVE void poc_debug_continue(void) {
    g_cpc.paused = false;
    g_cpc.step_once = false;
    g_debug_checkpoint_valid = false;
    g_debug_stop_reason = POC_DEBUG_RUNNING;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_step_in(void) {
    if (!g_cpc.paused)
        return -1;
    memcpy(&g_debug_checkpoint, &g_cpc, sizeof(g_cpc));
    g_debug_checkpoint_valid = true;
    g_cpc.step_once = true;
    g_debug_step_reason = POC_DEBUG_STEP_IN;
    g_debug_stop_reason = POC_DEBUG_RUNNING;
    return 0;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_step_back(void) {
    if (!g_cpc.paused || !g_debug_checkpoint_valid)
        return -1;
    CpcBreakpointManager breakpoint_manager = g_cpc.breakpoint_manager;
    memcpy(&g_cpc, &g_debug_checkpoint, sizeof(g_cpc));
    g_cpc.breakpoint_manager = breakpoint_manager;
    g_cpc.paused = true;
    g_cpc.step_once = false;
    g_debug_checkpoint_valid = false;
    g_debug_stop_reason = POC_DEBUG_STEP_BACK;
    poc_audio_reset();
    return 0;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_step_out(void) {
    if (!g_cpc.paused || g_debug_temp_bp_id != CPC_BREAKPOINT_INVALID_ID)
        return -1;
    u16 sp = g_cpc.cpu.sp;
    u16 target = (u16)mem_read(&g_cpc.mem, sp) |
                 ((u16)mem_read(&g_cpc.mem, (u16)(sp + 1)) << 8);
    CpcBreakpointId id = cpc_breakpoint_add(
        &g_cpc, target, CPC_BP_ANY, 0, CPC_BP_SOURCE_TEMPORARY);
    if (id == CPC_BREAKPOINT_INVALID_ID)
        return -2;
    g_debug_temp_bp_id = id;
    g_debug_temp_bp_addr = target;
    g_debug_temp_bp_reason = POC_DEBUG_STEP_OUT;
    g_debug_checkpoint_valid = false;
    g_debug_stop_reason = POC_DEBUG_RUNNING;
    g_cpc.paused = false;
    return target;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_next(void) {
    if (!g_cpc.paused || g_debug_temp_bp_id != CPC_BREAKPOINT_INVALID_ID)
        return -1;

    memcpy(&g_debug_checkpoint, &g_cpc, sizeof(g_cpc));
    g_debug_checkpoint_valid = true;

    u16 pc = g_cpc.cpu.pc;
    u8 opcode = mem_read(&g_cpc.mem, pc);
    bool call_or_rst = opcode == 0xCD || (opcode & 0xC7) == 0xC4 ||
                       (opcode & 0xC7) == 0xC7;
    if (!call_or_rst) {
        g_cpc.step_once = true;
        g_debug_step_reason = POC_DEBUG_NEXT;
        g_debug_stop_reason = POC_DEBUG_RUNNING;
        return 0;
    }

    for (int i = 0; i < 0x10000; i++)
        g_debug_dis_memory[i] = mem_read(&g_cpc.mem, (u16)i);
    char mnemonic[64];
    int bytes = z80dis(g_debug_dis_memory, pc, mnemonic, sizeof(mnemonic));
    if (bytes <= 0) bytes = 1;

    u16 target = (u16)(pc + bytes);
    CpcBreakpointId id = cpc_breakpoint_add(
        &g_cpc, target, CPC_BP_ANY, 0, CPC_BP_SOURCE_TEMPORARY);
    if (id == CPC_BREAKPOINT_INVALID_ID) {
        g_debug_checkpoint_valid = false;
        return -2;
    }

    g_debug_temp_bp_id = id;
    g_debug_temp_bp_addr = target;
    g_debug_temp_bp_reason = POC_DEBUG_NEXT;
    g_debug_stop_reason = POC_DEBUG_RUNNING;
    g_cpc.paused = false;
    return 0;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_is_paused(void) {
    return g_cpc.paused ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_stop_reason(void) {
    return g_debug_stop_reason;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_can_step_back(void) {
    return g_debug_checkpoint_valid ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_reg(int reg) {
    switch (reg) {
    case 0: return g_cpc.cpu.af;
    case 1: return g_cpc.cpu.bc;
    case 2: return g_cpc.cpu.de;
    case 3: return g_cpc.cpu.hl;
    case 4: return g_cpc.cpu.ix;
    case 5: return g_cpc.cpu.iy;
    case 6: return g_cpc.cpu.sp;
    case 7: return g_cpc.cpu.pc;
    case 8: return g_cpc.cpu.af_;
    case 9: return g_cpc.cpu.bc_;
    case 10: return g_cpc.cpu.de_;
    case 11: return g_cpc.cpu.hl_;
    case 12: return ((int)g_cpc.cpu.i << 8) | g_cpc.cpu.r;
    case 13: return g_cpc.cpu.im;
    default: return -1;
    }
}

EMSCRIPTEN_KEEPALIVE int poc_debug_breakpoint_set(int addr) {
    CpcBreakpointId id = cpc_breakpoint_add(
        &g_cpc, (u16)addr, CPC_BP_ANY, 0, CPC_BP_SOURCE_DAP);
    if (id == CPC_BREAKPOINT_INVALID_ID) return -1;
    if (id > INT_MAX) {
        cpc_breakpoint_clear(&g_cpc, id);
        return -1;
    }
    return (int)id;
}

EMSCRIPTEN_KEEPALIVE void poc_debug_breakpoint_clear(int id) {
    if (id <= 0) return;
    cpc_breakpoint_clear(&g_cpc, (CpcBreakpointId)id);
    if ((CpcBreakpointId)id == g_debug_temp_bp_id) {
        g_debug_temp_bp_id = CPC_BREAKPOINT_INVALID_ID;
        g_debug_temp_bp_reason = POC_DEBUG_RUNNING;
    }
}

EMSCRIPTEN_KEEPALIVE int poc_debug_breakpoint_enabled(int id) {
    const CpcBreakpoint *breakpoint = id > 0
        ? cpc_breakpoint_get(&g_cpc, (CpcBreakpointId)id) : NULL;
    return breakpoint && breakpoint->armed ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_breakpoint_addr(int id) {
    const CpcBreakpoint *breakpoint = id > 0
        ? cpc_breakpoint_get(&g_cpc, (CpcBreakpointId)id) : NULL;
    return breakpoint ? breakpoint->address : -1;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_breakpoint_count(void) {
    size_t count = cpc_breakpoint_count(&g_cpc);
    return count <= INT_MAX ? (int)count : INT_MAX;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_breakpoint_id_at(int index) {
    if (index < 0) return -1;
    const CpcBreakpoint *breakpoint =
        cpc_breakpoint_at(&g_cpc, (size_t)index);
    return breakpoint && breakpoint->id <= INT_MAX
        ? (int)breakpoint->id : -1;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_breakpoint_source(int id) {
    const CpcBreakpoint *breakpoint = id > 0
        ? cpc_breakpoint_get(&g_cpc, (CpcBreakpointId)id) : NULL;
    return breakpoint ? breakpoint->source : -1;
}

EMSCRIPTEN_KEEPALIVE void poc_set_snapshot_breakpoints(int enabled) {
    cpc_set_snapshot_breakpoints(&g_cpc, enabled != 0);
}

EMSCRIPTEN_KEEPALIVE int poc_snapshot_breakpoints(void) {
    return g_cpc.snapshot_breakpoints ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_mem_read(int addr) {
    return mem_read(&g_cpc.mem, (u16)addr);
}

EMSCRIPTEN_KEEPALIVE int poc_debug_mem_write_byte(int addr, int value) {
    if (!g_cpc.paused)
        return -1;
    poc_debug_mem_write(&g_cpc, (u16)addr, (u8)value);
    g_debug_checkpoint_valid = false;
    return 0;
}

EMSCRIPTEN_KEEPALIVE const char *poc_debug_disassemble(int addr, int lines) {
    if (lines < 1) lines = 1;
    if (lines > POC_DEBUG_DIS_LINES) lines = POC_DEBUG_DIS_LINES;
    for (int i = 0; i < 0x10000; i++)
        g_debug_dis_memory[i] = mem_read(&g_cpc.mem, (u16)i);

    size_t used = 0;
    u16 pc = (u16)addr;
    g_debug_disassembly[0] = '\0';
    for (int line = 0; line < lines; line++) {
        char mnemonic[64];
        char symbol[128];
        char comment[128];
        char annotation[256];
        int bytes = z80dis(g_debug_dis_memory, pc, mnemonic,
                           sizeof(mnemonic));
        if (bytes <= 0) bytes = 1;
        int written = snprintf(g_debug_disassembly + used,
                               sizeof(g_debug_disassembly) - used,
                               "%c%04X  ", pc == g_cpc.cpu.pc ? '>' : ' ', pc);
        if (written < 0 || (size_t)written >= sizeof(g_debug_disassembly) - used)
            break;
        used += (size_t)written;
        for (int i = 0; i < 4; i++) {
            written = snprintf(g_debug_disassembly + used,
                               sizeof(g_debug_disassembly) - used,
                               i < bytes ? "%02X " : "   ",
                               g_debug_dis_memory[(u16)(pc + i)]);
            if (written < 0 || (size_t)written >= sizeof(g_debug_disassembly) - used)
                return g_debug_disassembly;
            used += (size_t)written;
        }
        remu_symbol_format(&g_cpc.remu_debug, &g_cpc.mem, pc,
                           symbol, sizeof(symbol));
        if (!symbol[0])
            symbols_format(pc, g_cpc.mem.ram_bank, symbol, sizeof(symbol));
        remu_comment_format(&g_cpc.remu_debug, &g_cpc.mem, pc,
                            comment, sizeof(comment));
        if (symbol[0] && comment[0])
            snprintf(annotation, sizeof(annotation), "%s | %s", symbol, comment);
        else
            snprintf(annotation, sizeof(annotation), "%s%s", symbol, comment);
        written = snprintf(g_debug_disassembly + used,
                           sizeof(g_debug_disassembly) - used,
                           " %s%s%s\n", mnemonic,
                           annotation[0] ? " ; " : "", annotation);
        if (written < 0 || (size_t)written >= sizeof(g_debug_disassembly) - used)
            break;
        used += (size_t)written;
        pc = (u16)(pc + bytes);
    }
    return g_debug_disassembly;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_watch_set(int slot, int addr) {
    if (slot < 0 || slot >= POC_DEBUG_WATCH_MAX)
        return -1;
    g_debug_watch_addr[slot] = (u16)addr;
    g_debug_watch_enabled[slot] = true;
    return 0;
}

EMSCRIPTEN_KEEPALIVE void poc_debug_watch_clear(int slot) {
    if (slot >= 0 && slot < POC_DEBUG_WATCH_MAX)
        g_debug_watch_enabled[slot] = false;
}

static const PocDebugWriteEvent *poc_debug_event(unsigned int serial) {
    PocDebugWriteEvent *event = &g_debug_events[serial % POC_DEBUG_EVENT_MAX];
    return event->serial == serial ? event : NULL;
}

EMSCRIPTEN_KEEPALIVE unsigned int poc_debug_watch_serial(void) {
    return g_debug_event_serial;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_watch_event_slot(unsigned int serial) {
    const PocDebugWriteEvent *event = poc_debug_event(serial);
    return event ? event->watch_slot : -1;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_watch_event_addr(unsigned int serial) {
    const PocDebugWriteEvent *event = poc_debug_event(serial);
    return event ? event->addr : -1;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_watch_event_pc(unsigned int serial) {
    const PocDebugWriteEvent *event = poc_debug_event(serial);
    return event ? event->pc : -1;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_watch_event_old(unsigned int serial) {
    const PocDebugWriteEvent *event = poc_debug_event(serial);
    return event ? event->old_value : -1;
}

EMSCRIPTEN_KEEPALIVE int poc_debug_watch_event_new(unsigned int serial) {
    const PocDebugWriteEvent *event = poc_debug_event(serial);
    return event ? event->new_value : -1;
}

EMSCRIPTEN_KEEPALIVE unsigned int *poc_pixels(void) {
    return g_cpc.display.pixels;
}

EMSCRIPTEN_KEEPALIVE void poc_key(int scancode, int pressed) {
    kbd_sdl_key(&g_cpc.kbd, (SDL_Scancode)scancode, pressed != 0);
}

static bool poc_valid_drive(int drive) { return drive == 0 || drive == 1; }

EMSCRIPTEN_KEEPALIVE int poc_load_disk(int drive, const char *path) {
    if (!poc_valid_drive(drive) || !path)
        return -1;
    if (disk_load(&g_cpc.drive[drive], path) != 0)
        return -1;
    return 0;
}

EMSCRIPTEN_KEEPALIVE int poc_eject_disk(int drive) {
    if (!poc_valid_drive(drive))
        return -1;
    disk_eject(&g_cpc.drive[drive]);
    return 0;
}

EMSCRIPTEN_KEEPALIVE int poc_disk_inserted(int drive) {
    return poc_valid_drive(drive) && g_cpc.drive[drive].inserted ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int poc_tape_load(const char *path) {
    if (!tape_load(&g_cpc.tape, path))
        return -1;
    tape_set_paused(&g_cpc.tape, true);
    poc_audio_reset();
    return 0;
}

EMSCRIPTEN_KEEPALIVE void poc_tape_eject(void) {
    tape_eject(&g_cpc.tape);
    poc_audio_reset();
}

EMSCRIPTEN_KEEPALIVE int poc_tape_play(void) {
    if (!tape_loaded(&g_cpc.tape) || g_cpc.tape.stage == TAPE_END)
        return -1;
    tape_set_paused(&g_cpc.tape, false);
    return 0;
}

EMSCRIPTEN_KEEPALIVE void poc_tape_stop(void) {
    tape_set_paused(&g_cpc.tape, true);
}

EMSCRIPTEN_KEEPALIVE void poc_tape_rewind(void) {
    tape_set_paused(&g_cpc.tape, true);
    tape_rewind(&g_cpc.tape);
}

EMSCRIPTEN_KEEPALIVE void poc_tape_next(void) {
    tape_set_paused(&g_cpc.tape, true);
    tape_next_block(&g_cpc.tape);
}

EMSCRIPTEN_KEEPALIVE int poc_tape_loaded(void) {
    return tape_loaded(&g_cpc.tape) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int poc_tape_motor(void) {
    return g_cpc.tape.motor ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int poc_tape_playing(void) {
    return tape_playing(&g_cpc.tape) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int poc_tape_paused(void) {
    return g_cpc.tape.paused ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int poc_tape_ended(void) {
    return g_cpc.tape.stage == TAPE_END ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int poc_tape_counter(void) {
    return (int)(tape_counter_seconds(&g_cpc.tape) % 1000u);
}

/* Queue RUN"filename" after a frame-counted boot delay. This mirrors the
 * native --autostart path instead of synthesizing browser key events. */
EMSCRIPTEN_KEEPALIVE int poc_autorun(const char *filename, int delay_frames) {
    if (!filename || !filename[0])
        return -1;
    size_t len = strlen(filename);
    if (len > 240)
        return -1;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)filename[i];
        if (c < 0x20 || c == 0x7f || c == '"')
            return -1;
    }

    poc_cancel_paste();
    int written = snprintf(g_autorun_command, sizeof(g_autorun_command),
                           "run\"%s", filename);
    if (written < 0 || written >= (int)sizeof(g_autorun_command)) {
        g_autorun_command[0] = '\0';
        return -1;
    }
    g_autorun_frames = delay_frames > 0 ? delay_frames : 42;
    return 0;
}

/* CPC joystick 1 = keyboard matrix row 9, bits 0-5 (Up Down Left Right F1 F2). */
#define POC_JOY_ROW 9
EMSCRIPTEN_KEEPALIVE void poc_joy(int col, int pressed) {
    if (col < 0 || col > 5) return;
    if (pressed) kbd_key_down(&g_cpc.kbd, POC_JOY_ROW, col);
    else         kbd_key_up  (&g_cpc.kbd, POC_JOY_ROW, col);
}

/* Diagnostic readback used by the browser to distinguish Gamepad API mapping
 * failures from CPC-side input failures. Row 9 is active low. */
EMSCRIPTEN_KEEPALIVE int poc_joy_matrix(void) {
    return g_cpc.kbd.matrix[POC_JOY_ROW];
}

/* Browser pointer input uses the AMX adapter on joystick port 1. The AMX
 * mouse and a physical joystick share matrix row 9, so the web UI presents
 * them as mutually exclusive input channels. */
EMSCRIPTEN_KEEPALIVE void poc_set_mouse(int enabled) {
    amx_reset(&g_cpc.amx, &g_cpc.kbd);
    g_cpc.amx_mouse = enabled != 0;
}

EMSCRIPTEN_KEEPALIVE void poc_mouse_move(int dx, int dy) {
    if (g_cpc.amx_mouse)
        amx_move(&g_cpc.amx, dx, dy);
}

EMSCRIPTEN_KEEPALIVE void poc_mouse_button(int button, int pressed) {
    if (g_cpc.amx_mouse)
        amx_button(&g_cpc.amx, &g_cpc.kbd, button, pressed != 0);
}

/* The CPC motor signal is shared. Attribute it to the drive selected by the
 * current or most recently completed FDC command for the front-panel LEDs. */
EMSCRIPTEN_KEEPALIVE int poc_disk_motor(int drive) {
    if (!poc_valid_drive(drive) || !g_cpc.fdc.motor)
        return 0;
    return (g_cpc.fdc.cmd[1] & 0x01) == drive ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE int poc_width(void)  { return CPC_SCREEN_W; }
EMSCRIPTEN_KEEPALIVE int poc_height(void) { return CPC_SCREEN_H; }

EMSCRIPTEN_KEEPALIVE const char *poc_build_version(void) {
    return PACKAGE_VERSION;
}

EMSCRIPTEN_KEEPALIVE const char *poc_build_commit(void) {
    return PROG_GIT_COMMIT;
}

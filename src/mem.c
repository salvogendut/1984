#include "mem.h"
#include <stdio.h>
#include <string.h>

/* Read a 16 KB ROM image into `dest`, transparently skipping a 128-byte
 * AMSDOS header if the file size indicates one is present (16384+128 bytes). */
static int read_rom_image(FILE *f, u8 *dest) {
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz == ROM_BASIC_SIZE + 128)
        fseek(f, 128, SEEK_SET);
    return (int)fread(dest, 1, ROM_BASIC_SIZE, f);
}

void mem_init(Mem *m) {
    memset(m->ram, 0, sizeof(m->ram));
    memset(m->rom_os, 0, sizeof(m->rom_os));
    memset(m->rom_basic, 0, sizeof(m->rom_basic));
    memset(m->rom_amsdos, 0xFF, sizeof(m->rom_amsdos));
    m->amsdos_present    = false;
    memset(m->rom_ext, 0xFF, sizeof(m->rom_ext));
    memset(m->rom_ext_present, 0, sizeof(m->rom_ext_present));
    m->lower_rom_enabled = true;
    m->upper_rom_enabled = true;
    m->upper_rom_select  = 0;
    m->ram_bank          = 0;
    m->ram_size          = 0x20000;  /* default 128 KB; caller sets from config */
}

int mem_load_os(Mem *m, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open OS ROM: %s\n", path); return -1; }
    read_rom_image(f, m->rom_os);
    fclose(f);
    return 0;
}

int mem_load_rom(Mem *m, const char *os_path, const char *basic_path) {
    FILE *f;

    f = fopen(os_path, "rb");
    if (!f) { fprintf(stderr, "Cannot open OS ROM: %s\n", os_path); return -1; }
    read_rom_image(f, m->rom_os);
    fclose(f);

    f = fopen(basic_path, "rb");
    if (!f) { fprintf(stderr, "Cannot open BASIC ROM: %s\n", basic_path); return -1; }
    read_rom_image(f, m->rom_basic);
    fclose(f);

    return 0;
}

int mem_load_amsdos(Mem *m, const char *path) {
    if (!path || !path[0]) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open AMSDOS ROM: %s\n", path); return -1; }
    read_rom_image(f, m->rom_amsdos);
    fclose(f);
    m->amsdos_present = true;
    return 0;
}

void mem_unload_amsdos(Mem *m) {
    memset(m->rom_amsdos, 0xFF, sizeof(m->rom_amsdos));
    m->amsdos_present = false;
}

int mem_load_rom_ext(Mem *m, int slot, const char *path) {
    if (slot < 0 || slot >= ROM_EXT_COUNT) return -1;
    if (!path || !path[0]) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open ROM slot %d: %s\n", slot, path); return -1; }
    read_rom_image(f, m->rom_ext[slot]);
    fclose(f);
    m->rom_ext_present[slot] = true;
    return 0;
}

void mem_unload_rom_ext(Mem *m, int slot) {
    if (slot < 0 || slot >= ROM_EXT_COUNT) return;
    memset(m->rom_ext[slot], 0xFF, ROM_BASIC_SIZE);
    m->rom_ext_present[slot] = false;
}

/* Translate a Z80 address to its physical RAM offset under the current
 * Gate Array banking configuration.  Only called when ram_bank != 0.
 *
 * The CPC 6128 Gate Array banking byte (bits[7:6] = 11) encodes:
 *   bits[2:0]  banking mode 0-7 — selects which of 8 page layouts to use
 *   bits[5:3]  expansion bank   — DK'tronics: which 64 KB block maps to
 *                                 romb4-romb7 (0 = standard 6128 extra 64 KB)
 *
 * Page layout per mode (from Caprice32 ga_init_banking):
 *   base pages:   romb0=0x00000  romb1=0x04000  romb2=0x08000  romb3=0x0C000
 *   extra pages:  romb4=X+0x0    romb5=X+0x4000 romb6=X+0x8000 romb7=X+0xC000
 *                 where X = (expansion_bank + 1) * 0x10000
 *
 *   mode | 0x0000  0x4000  0x8000  0xC000
 *   -----+------------------------------------
 *     0  | rb0     rb1     rb2     rb3
 *     1  | rb0     rb1     rb2     rb7   ← most common: extra 0xC000 page
 *     2  | rb4     rb5     rb6     rb7
 *     3  | rb0     rb3     rb2     rb7
 *     4  | rb0     rb4     rb2     rb3
 *     5  | rb0     rb5     rb2     rb3
 *     6  | rb0     rb6     rb2     rb3
 *     7  | rb0     rb7     rb2     rb3
 *
 * Read vs. write asymmetry: upper ROM always overlays the 0xC000 read path
 * (handled in mem_read); writes always go to the banked RAM page (here).
 * Video reads (mem_read_video) bypass ROM and use this function directly. */
static u32 banked_ram_offset(const Mem *m, u16 addr) {
    u8  mode      = m->ram_bank & 0x07;
    u8  group     = (m->ram_bank >> 3) & 0x07;
    u8  bank_high = (m->ram_bank >> 6) & 0x03;  /* Yarek upper bank group (0=DK'tronics) */
    u32 full_bg   = (u32)bank_high * 8u + group;
    u32 extra     = (full_bg + 1u) * 0x10000u;  /* start of romb4-romb7 */

    if (addr < 0x4000) {
        return (mode == 2) ? extra + (u32)addr : (u32)addr;
    }
    if (addr < 0x8000) {
        u32 rel = (u32)(addr - 0x4000u);
        switch (mode) {
        case 2: return extra + 0x4000u + rel;
        case 3: return 0x0C000u + rel;
        case 4: return extra + 0x0000u + rel;
        case 5: return extra + 0x4000u + rel;
        case 6: return extra + 0x8000u + rel;
        case 7: return extra + 0xC000u + rel;
        default: return (u32)addr;
        }
    }
    if (addr < 0xC000) {
        u32 rel = (u32)(addr - 0x8000u);
        return (mode == 2) ? extra + 0x8000u + rel : (u32)addr;
    }
    /* 0xC000-0xFFFF: romb7 in modes 1/2/3, else romb3 (standard) */
    {
        u32 rel = (u32)(addr - 0xC000u);
        return (mode == 1 || mode == 2 || mode == 3)
               ? extra + 0xC000u + rel
               : (u32)addr;
    }
}

static inline u8 read_ram(const Mem *m, u32 off) {
    return (off < (u32)m->ram_size) ? m->ram[off] : 0xFF;
}

u8 mem_read(Mem *m, u16 addr) {
    /* Lower ROM overlay */
    if (addr < 0x4000 && m->lower_rom_enabled)
        return m->rom_os[addr];

    /* Upper ROM overlay — always wins on reads at 0xC000 when enabled,
     * even when banking is active (writes still go to banked RAM). */
    if (addr >= 0xC000 && m->upper_rom_enabled) {
        u8 slot = m->upper_rom_select;
        if (slot < ROM_EXT_COUNT && m->rom_ext_present[slot])
            return m->rom_ext[slot][addr - 0xC000];
        if (slot == 0)
            return m->rom_basic[addr - 0xC000];
        if (slot == 7 && m->amsdos_present)
            return m->rom_amsdos[addr - 0xC000];
        return 0xFF;
    }

    /* RAM read — apply banking for all regions when active */
    if (m->ram_bank)
        return read_ram(m, banked_ram_offset(m, addr));
    return m->ram[addr];
}

u8 mem_read_video(Mem *m, u16 addr) {
    /* CPC video hardware is hardwired to the base 64 KB of physical RAM.
     * GA banking only re-routes CPU address decoding; the video scanning
     * circuit always reads from the unbanked physical address.
     * Applying banked_ram_offset here was wrong and caused scan-line
     * corruption whenever software switched banks mid-frame. */
    return m->ram[(u32)addr];
}

void mem_write(Mem *m, u16 addr, u8 val) {
    /* Writes always go to RAM at the banked page; ROM overlay never intercepts writes */
    u32 off = m->ram_bank ? banked_ram_offset(m, addr) : (u32)addr;
    if (off < (u32)m->ram_size)
        m->ram[off] = val;
}

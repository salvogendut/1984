#include "mem.h"
#include <stdio.h>
#include <string.h>

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
    fread(m->rom_os, 1, ROM_OS_SIZE, f);
    fclose(f);
    return 0;
}

int mem_load_rom(Mem *m, const char *os_path, const char *basic_path) {
    FILE *f;

    f = fopen(os_path, "rb");
    if (!f) { fprintf(stderr, "Cannot open OS ROM: %s\n", os_path); return -1; }
    fread(m->rom_os, 1, ROM_OS_SIZE, f);
    fclose(f);

    f = fopen(basic_path, "rb");
    if (!f) { fprintf(stderr, "Cannot open BASIC ROM: %s\n", basic_path); return -1; }
    fread(m->rom_basic, 1, ROM_BASIC_SIZE, f);
    fclose(f);

    return 0;
}

int mem_load_amsdos(Mem *m, const char *path) {
    if (!path || !path[0]) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open AMSDOS ROM: %s\n", path); return -1; }
    fread(m->rom_amsdos, 1, ROM_BASIC_SIZE, f);
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
    fread(m->rom_ext[slot], 1, ROM_BASIC_SIZE, f);
    fclose(f);
    m->rom_ext_present[slot] = true;
    return 0;
}

void mem_unload_rom_ext(Mem *m, int slot) {
    if (slot < 0 || slot >= ROM_EXT_COUNT) return;
    memset(m->rom_ext[slot], 0xFF, ROM_BASIC_SIZE);
    m->rom_ext_present[slot] = false;
}

/* Compute the RAM offset for the banked 0xC000-0xFFFF region.
 * Bits[5:0] of ram_bank form a 6-bit 16KB-page index (0-63):
 *   page 0-7   = standard 128 KB (6128 built-in)
 *   page 8-63  = DK'tronics-style expansion (up to 1 MB total)
 * Returns RAM_SIZE if the page exceeds ram_size (treat as uninstalled). */
static inline u32 banked_offset(const Mem *m, u16 addr) {
    u32 page = (u32)(m->ram_bank & 0x3F);
    u32 off  = page * 0x4000u + (u32)(addr - 0xC000u);
    return (off < (u32)m->ram_size) ? off : RAM_SIZE;
}

u8 mem_read(Mem *m, u16 addr) {
    if (addr < 0x4000 && m->lower_rom_enabled)
        return m->rom_os[addr];
    if (addr >= 0xC000 && m->upper_rom_enabled) {
        /* RAM bank takes priority over ROM when banking is active */
        if (m->ram_bank) {
            u32 off = banked_offset(m, addr);
            return (off < RAM_SIZE) ? m->ram[off] : 0xFF;
        }
        /* Expansion ROM overrides take priority; fall back to BASIC/AMSDOS defaults */
        u8 slot = m->upper_rom_select;
        if (slot < ROM_EXT_COUNT && m->rom_ext_present[slot])
            return m->rom_ext[slot][addr - 0xC000];
        if (slot == 0)
            return m->rom_basic[addr - 0xC000];
        if (slot == 7 && m->amsdos_present)
            return m->rom_amsdos[addr - 0xC000];
        return 0xFF;
    }
    if (addr >= 0xC000 && m->ram_bank) {
        u32 off = banked_offset(m, addr);
        return (off < RAM_SIZE) ? m->ram[off] : 0xFF;
    }
    return m->ram[addr];
}

u8 mem_read_video(Mem *m, u16 addr) {
    if (addr >= 0xC000 && m->ram_bank) {
        u32 off = banked_offset(m, addr);
        return (off < RAM_SIZE) ? m->ram[off] : 0xFF;
    }
    return m->ram[addr];
}

void mem_write(Mem *m, u16 addr, u8 val) {
    if (addr >= 0xC000 && m->ram_bank) {
        u32 off = banked_offset(m, addr);
        if (off < RAM_SIZE) m->ram[off] = val;
        return;
    }
    m->ram[addr] = val;
}

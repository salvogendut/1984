#include "mem.h"
#include <stdio.h>
#include <string.h>

void mem_init(Mem *m) {
    memset(m->ram, 0, sizeof(m->ram));
    memset(m->rom_os, 0, sizeof(m->rom_os));
    memset(m->rom_basic, 0, sizeof(m->rom_basic));
    m->lower_rom_enabled = true;
    m->upper_rom_enabled = true;
    m->ram_bank = 0;
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

u8 mem_read(Mem *m, u16 addr) {
    if (addr < 0x4000 && m->lower_rom_enabled)
        return m->rom_os[addr];
    if (addr >= 0xC000 && m->upper_rom_enabled)
        return m->rom_basic[addr - 0xC000];
    /* 6128 banking: bits 0-2 of ram_bank select 16 KB page at 0xC000 */
    if (addr >= 0xC000 && m->ram_bank) {
        u32 page = m->ram_bank & 0x07;
        return m->ram[(page * 0x4000) + (addr - 0xC000)];
    }
    return m->ram[addr & (RAM_SIZE - 1)];
}

void mem_write(Mem *m, u16 addr, u8 val) {
    if (addr >= 0xC000 && m->ram_bank) {
        u32 page = m->ram_bank & 0x07;
        m->ram[(page * 0x4000) + (addr - 0xC000)] = val;
        return;
    }
    m->ram[addr & (RAM_SIZE - 1)] = val;
}

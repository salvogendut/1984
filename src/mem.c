#include "mem.h"
#include <stdio.h>
#include <stdlib.h>
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
    m->snapshot_lower_rom = NULL;
    memset(m->snapshot_upper_rom, 0, sizeof(m->snapshot_upper_rom));
    m->lower_rom_enabled = true;
    m->upper_rom_enabled = true;
    m->upper_rom_select  = 0;
    m->ram_bank          = 0;
    m->ram_size          = 0x20000;  /* default 128 KB; caller sets from config */
    /* Populate the M4 snapshot-ROM stub: FUZIX reads two bytes —
     * 'M' (0x4D) at 0x100 (the "MV - SNA" header), and a rom slot
     * number at 0x0. Filling rest with 0xFF mirrors blank ROM. */
    memset(m->m4_snapshot_rom_stub, 0xFF, sizeof(m->m4_snapshot_rom_stub));
    m->m4_snapshot_rom_stub[0x000] = 0x06;  /* M4_ROM_SLOT in m4.h — must match where M4ROM.ROM loads */
    m->m4_snapshot_rom_stub[0x100] = 0x4D;  /* 'M' — start of "MV - SNA" */
    m->lower_rom_override = NULL;
    cartridge_init(&m->cartridge);
    m->plus = false;
    memset(m->plus_registers, 0, sizeof(m->plus_registers));
    mem_plus_reset_mapping(m);
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

    m->plus = false;
    mem_plus_reset_mapping(m);

    return 0;
}

int mem_load_cartridge(Mem *m, const char *path) {
    Cartridge *loaded = malloc(sizeof(*loaded));
    if (!loaded) {
        fprintf(stderr, "Cannot allocate CPR cartridge buffer\n");
        return -1;
    }
    if (cartridge_load(loaded, path) < 0) {
        free(loaded);
        return -1;
    }
    if (!cartridge_page_present(loaded, 0)) {
        fprintf(stderr, "Cartridge has no boot page cb00: %s\n", path);
        free(loaded);
        return -1;
    }

    m->cartridge = *loaded;
    free(loaded);
    m->plus = true;
    mem_plus_reset_mapping(m);

    /* Existing OCR and firmware helpers consume these buffers directly.
     * Keep them coherent with the standard Plus system-cartridge layout. */
    memset(m->rom_os, 0xFF, sizeof(m->rom_os));
    memset(m->rom_basic, 0xFF, sizeof(m->rom_basic));
    memset(m->rom_amsdos, 0xFF, sizeof(m->rom_amsdos));
    m->amsdos_present = false;
    memcpy(m->rom_os, cartridge_page(&m->cartridge, 0), ROM_OS_SIZE);
    if (cartridge_page_present(&m->cartridge, 1))
        memcpy(m->rom_basic, cartridge_page(&m->cartridge, 1), ROM_BASIC_SIZE);
    if (cartridge_page_present(&m->cartridge, 3)) {
        memcpy(m->rom_amsdos, cartridge_page(&m->cartridge, 3), ROM_BASIC_SIZE);
        m->amsdos_present = true;
    }
    return 0;
}

void mem_plus_reset_mapping(Mem *m) {
    m->plus_lower_bank = 0;
    m->plus_lower_page = 0;
    m->plus_register_page = false;
    m->upper_rom_select = m->plus ? 1 : 0;
}

void mem_plus_set_rmr2(Mem *m, u8 value) {
    if (!m->plus) return;
    u8 bank = (value >> 3) & 0x03;
    m->plus_register_page = bank == 3;
    m->plus_lower_bank = m->plus_register_page ? 0 : bank;
    m->plus_lower_page = value & 0x07;
}

void mem_plus_select_upper_rom(Mem *m, u8 value) {
    if (!m->plus) {
        m->upper_rom_select = value;
        return;
    }
    if (value == 7)
        m->upper_rom_select = 3;
    else if (value >= 128)
        m->upper_rom_select = value & 31;
    else
        m->upper_rom_select = 1;
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

int mem_set_snapshot_rom(Mem *m, int bank, const u8 *data) {
    if (!m || !data || bank < 0 || bank > SNAPSHOT_ROM_COUNT)
        return -1;
    u8 **target = bank == SNAPSHOT_ROM_COUNT
                ? &m->snapshot_lower_rom : &m->snapshot_upper_rom[bank];
    u8 *copy = malloc(ROM_BASIC_SIZE);
    if (!copy) return -1;
    memcpy(copy, data, ROM_BASIC_SIZE);
    free(*target);
    *target = copy;
    return 0;
}

const u8 *mem_get_snapshot_rom(const Mem *m, int bank) {
    if (!m || bank < 0 || bank > SNAPSHOT_ROM_COUNT) return NULL;
    return bank == SNAPSHOT_ROM_COUNT
         ? m->snapshot_lower_rom : m->snapshot_upper_rom[bank];
}

void mem_clear_snapshot_roms(Mem *m) {
    if (!m) return;
    free(m->snapshot_lower_rom);
    m->snapshot_lower_rom = NULL;
    for (int slot = 0; slot < SNAPSHOT_ROM_COUNT; slot++) {
        free(m->snapshot_upper_rom[slot]);
        m->snapshot_upper_rom[slot] = NULL;
    }
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
u32 mem_ram_offset_for_config(u8 ram_bank, u16 addr) {
    u8  mode      = ram_bank & 0x07;
    u8  group     = (ram_bank >> 3) & 0x07;
    u8  bank_high = (ram_bank >> 6) & 0x03;  /* Yarek upper bank group (0=DK'tronics) */
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

u32 mem_ram_offset(const Mem *m, u16 addr) {
    return mem_ram_offset_for_config(m->ram_bank, addr);
}

int mem_visible_rom_bank(const Mem *m, u16 addr) {
    if (m->plus && m->lower_rom_enabled &&
            (addr >> 14) == m->plus_lower_bank)
        return m->plus_lower_page;
    if (!m->plus && addr < 0x4000 && m->lower_rom_enabled)
        return 256; /* RASM's conventional lower-ROM bank number. */
    if (addr >= 0xC000 && m->upper_rom_enabled)
        return m->upper_rom_select;
    return -1;
}

int mem_visible_ram_bank(const Mem *m, u16 addr) {
    if (m->plus && m->plus_register_page && addr >= 0x4000 && addr < 0x8000)
        return -1;
    if (mem_visible_rom_bank(m, addr) >= 0)
        return -1;
    u32 off = mem_ram_offset(m, addr);
    return off < (u32)m->ram_size ? (int)(off >> 14) : -1;
}

static inline u8 read_ram(const Mem *m, u32 off) {
    return (off < (u32)m->ram_size) ? m->ram[off] : 0xFF;
}

u8 mem_read(Mem *m, u16 addr) {
    if (m->plus && m->plus_register_page && addr >= 0x4000 && addr < 0x8000)
        return m->plus_registers[addr - 0x4000];

    if (m->plus && m->lower_rom_enabled &&
            (addr >> 14) == m->plus_lower_bank) {
        const u8 *page = cartridge_page(&m->cartridge, m->plus_lower_page);
        return page ? page[addr & 0x3FFF] : 0xFF;
    }

    /* Classic CPC lower ROM overlay. On Plus machines RMR2 moves the
     * cartridge overlay to plus_lower_bank; it must not remain duplicated
     * at 0x0000 when another bank is selected. */
    if (!m->plus && addr < 0x4000 && m->lower_rom_enabled) {
        if (m->lower_rom_override)
            return m->lower_rom_override[addr];
        if (m->snapshot_lower_rom)
            return m->snapshot_lower_rom[addr];
        return m->rom_os[addr];
    }

    /* Upper ROM overlay — always wins on reads at 0xC000 when enabled,
     * even when banking is active (writes still go to banked RAM). */
    if (addr >= 0xC000 && m->upper_rom_enabled) {
        u8 slot = m->upper_rom_select;
        if (m->snapshot_upper_rom[slot])
            return m->snapshot_upper_rom[slot][addr - 0xC000];
        if (m->plus) {
            const u8 *page = cartridge_page(&m->cartridge, slot);
            if (!page) page = cartridge_page(&m->cartridge, 1);
            return page ? page[addr - 0xC000] : 0xFF;
        }
        if (slot < ROM_EXT_COUNT && m->rom_ext_present[slot])
            return m->rom_ext[slot][addr - 0xC000];
        if (slot == 0)
            return m->rom_basic[addr - 0xC000];
        if (slot == 7 && m->amsdos_present)
            return m->rom_amsdos[addr - 0xC000];
        /* On a stock CPC, selecting an unpopulated expansion ROM leaves the
         * internal BASIC ROM visible. Caprice32 mirrors this by falling back
         * to pbROMhi when memmap_ROM[slot] is null. The CP/M Plus loader
         * probes ROM 0xFF and expects to see BASIC's header there. */
        return m->rom_basic[addr - 0xC000];
    }

    /* RAM read — apply banking for all regions when active */
    if (m->ram_bank)
        return read_ram(m, mem_ram_offset(m, addr));
    return m->ram[addr];
}

u8 mem_read_video(const Mem *m, u16 addr) {
    /* CPC video hardware is hardwired to the base 64 KB of physical RAM.
     * GA banking only re-routes CPU address decoding; the video scanning
     * circuit always reads from the unbanked physical address.
     * Applying banked_ram_offset here was wrong and caused scan-line
     * corruption whenever software switched banks mid-frame. */
    return m->ram[(u32)addr];
}

u8 mem_read_dma(Mem *m, u16 addr) {
    /* Plus DMA source addresses are physical base-RAM addresses. The ASIC
     * bypasses both ROM overlays and the CPU's Gate Array RAM mapping. */
    return read_ram(m, (u32)addr);
}

void mem_write(Mem *m, u16 addr, u8 val) {
    if (m->plus && m->plus_register_page && addr >= 0x4000 && addr < 0x8000) {
        m->plus_registers[addr - 0x4000] = val;
        return;
    }
    /* Writes always go to RAM at the banked page; ROM overlay never intercepts writes */
    u32 off = mem_ram_offset(m, addr);
    if (off < (u32)m->ram_size)
        m->ram[off] = val;
}

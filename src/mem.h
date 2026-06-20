#pragma once
#include "types.h"

/* CPC memory map:
 *   0x0000-0x3FFF  lower ROM (OS) or RAM bank
 *   0x4000-0x7FFF  RAM
 *   0x8000-0xBFFF  RAM
 *   0xC000-0xFFFF  upper ROM (selected by port 0xDFxx) or RAM bank
 *
 * Upper ROM slots: 0=BASIC, 7=AMSDOS. All others return 0xFF.
 * 464: 64 KB RAM, no extra banks
 * 6128: 128 KB RAM, banked via Gate Array (modes 0-7)
 * Expansion — DK'tronics (≤576 KB): port 0x7Fxx, data bits[5:3] select bank group.
 *   bits[5:3]=0 → standard 6128 extra 64 KB (RAM banks at offset 64–128 KB)
 *   bits[5:3]=1–7 → 7 × 64 KB expansion banks (128–576 KB)
 * Expansion — Yarek/RAM7 (>576 KB): port address bits A10–A8 carry the upper
 *   bank group (bank_high). Port 0x7Fxx = bank_high 0 (DK'tronics compatible).
 *   Port 0x7Exx = bank_high 1 (576–1088 KB), 0x7Dxx = 2, 0x7Cxx = 3.
 *   Full bank group = bank_high*8 + data_bits[5:3]; max 1024 KB with bank_high≤1.
 */

#define ROM_OS_SIZE    0x4000
#define ROM_BASIC_SIZE 0x4000
#define RAM_SIZE       0x100000  /* 1024 KB max (Yarek ceiling); actual usable size is Mem.ram_size */
#define ROM_EXT_COUNT  32        /* expansion ROM slots 0-31 */

typedef struct {
    u8   ram[RAM_SIZE];
    int  ram_size;              /* actual usable RAM in bytes (from config.memory_kb * 1024) */
    u8   rom_os[ROM_OS_SIZE];
    u8   rom_basic[ROM_BASIC_SIZE];
    u8   rom_amsdos[ROM_BASIC_SIZE];
    bool amsdos_present;
    /* expansion ROMs: slot 0=BASIC fallback, 7=AMSDOS fallback, 1-6/8-31 free */
    u8   rom_ext[ROM_EXT_COUNT][ROM_BASIC_SIZE];
    bool rom_ext_present[ROM_EXT_COUNT];
    bool lower_rom_enabled;
    bool upper_rom_enabled;
    u8   upper_rom_select;  /* current upper ROM slot (written via port 0xDFxx) */
    u8   ram_bank;          /* 6128 banking config (Gate Array port 0x7F) */
    /* When non-NULL and the lower ROM is enabled, reads in 0x0000-0x3FFF
     * come from this buffer instead of rom_os. Used by the M4 board's
     * C_ROMLOW (0x433D) "hack low rom" mode that FUZIX v2.0.7+ uses to
     * detect the board (reads byte 0x100, expects 'M' — start of the
     * "MV - SNA" snapshot header) and to fetch the M4 rom slot number
     * from offset 0x0. Set/cleared by m4.c's C_ROMLOW handler. */
    const u8 *lower_rom_override;
    /* Stub "M4 snapshot ROM" used by FUZIX detection. Real M4 firmware
     * exposes a full snapshot loader here; FUZIX only reads two bytes
     * (rom number at 0x0, 'M' identifier at 0x100) so a 16 KB blob with
     * just those is enough to pass detection and let FUZIX proceed to
     * use C_SDREAD/C_SDWRITE, which we already emulate. */
    u8   m4_snapshot_rom_stub[ROM_OS_SIZE];
} Mem;

void mem_init(Mem *m);
int  mem_load_os(Mem *m, const char *path);            /* reload lower ROM only */
int  mem_load_rom(Mem *m, const char *os_path, const char *basic_path);
int  mem_load_amsdos(Mem *m, const char *path);        /* slot 7 default; non-fatal */
void mem_unload_amsdos(Mem *m);                        /* clear slot 7 default */
int  mem_load_rom_ext(Mem *m, int slot, const char *path);  /* expansion slot 0-31 */
void mem_unload_rom_ext(Mem *m, int slot);
u8   mem_read(Mem *m, u16 addr);
u8   mem_read_video(Mem *m, u16 addr);   /* CRTC/GA always reads RAM, bypasses ROM */
void mem_write(Mem *m, u16 addr, u8 val);

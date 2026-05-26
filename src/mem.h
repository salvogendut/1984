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
 * 6128: 128 KB RAM, banked via Gate Array
 */

#define ROM_OS_SIZE    0x4000
#define ROM_BASIC_SIZE 0x4000
#define RAM_SIZE       0x20000   /* 128 KB (6128); 464 uses lower 64 KB */
#define ROM_EXT_COUNT  32        /* expansion ROM slots 0-31 */

typedef struct {
    u8   ram[RAM_SIZE];
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

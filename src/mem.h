#pragma once
#include "types.h"

/* CPC memory map:
 *   0x0000-0x3FFF  lower ROM (OS) or RAM bank
 *   0x4000-0x7FFF  RAM
 *   0x8000-0xBFFF  RAM
 *   0xC000-0xFFFF  upper ROM (BASIC) or RAM bank
 *
 * 464: 64 KB RAM, no extra banks
 * 6128: 128 KB RAM, banked via Gate Array
 */

#define ROM_OS_SIZE    0x4000
#define ROM_BASIC_SIZE 0x4000
#define RAM_SIZE       0x20000   /* 128 KB (6128); 464 uses lower 64 KB */

typedef struct {
    u8 ram[RAM_SIZE];
    u8 rom_os[ROM_OS_SIZE];
    u8 rom_basic[ROM_BASIC_SIZE];
    bool lower_rom_enabled;
    bool upper_rom_enabled;
    u8   ram_bank;    /* 6128 banking config (Gate Array port 0x7F) */
} Mem;

void mem_init(Mem *m);
int  mem_load_rom(Mem *m, const char *os_path, const char *basic_path);
u8   mem_read(Mem *m, u16 addr);
void mem_write(Mem *m, u16 addr, u8 val);

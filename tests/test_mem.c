#include "mem.h"

#include <assert.h>
#include <string.h>

static void seed_basic_header(Mem *mem) {
    memset(mem->rom_basic, 0xA5, sizeof(mem->rom_basic));
    mem->rom_basic[0] = 0x80;
    mem->rom_basic[1] = 0x01;
    mem->rom_basic[2] = 0x02;
}

static void test_absent_upper_rom_falls_back_to_basic(void) {
    Mem mem;
    mem_init(&mem);
    seed_basic_header(&mem);

    mem.upper_rom_enabled = true;
    mem.upper_rom_select = 0xFF;

    assert(mem_read(&mem, 0xC000) == 0x80);
    assert(mem_read(&mem, 0xC001) == 0x01);
    assert(mem_read(&mem, 0xC002) == 0x02);
}

static void test_present_extension_rom_wins(void) {
    Mem mem;
    mem_init(&mem);
    seed_basic_header(&mem);

    mem.upper_rom_enabled = true;
    mem.upper_rom_select = 5;
    mem.rom_ext_present[5] = true;
    mem.rom_ext[5][0] = 0x42;

    assert(mem_read(&mem, 0xC000) == 0x42);
}

static void test_amsdos_slot_still_wins(void) {
    Mem mem;
    mem_init(&mem);
    seed_basic_header(&mem);

    mem.upper_rom_enabled = true;
    mem.upper_rom_select = 7;
    mem.amsdos_present = true;
    mem.rom_amsdos[0] = 0x01;

    assert(mem_read(&mem, 0xC000) == 0x01);
}

static void test_disabled_upper_rom_reads_ram(void) {
    Mem mem;
    mem_init(&mem);
    seed_basic_header(&mem);

    mem.upper_rom_enabled = false;
    mem.upper_rom_select = 0xFF;
    mem.ram[0xC000] = 0x55;

    assert(mem_read(&mem, 0xC000) == 0x55);
}

int main(void) {
    test_absent_upper_rom_falls_back_to_basic();
    test_present_extension_rom_wins();
    test_amsdos_slot_still_wins();
    test_disabled_upper_rom_reads_ram();
    return 0;
}

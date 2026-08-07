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

static void seed_plus_page(Mem *mem, int page, u8 value) {
    mem->cartridge.present[page] = true;
    memset(mem->cartridge.page[page], value, CARTRIDGE_PAGE_SIZE);
}

static void test_plus_cartridge_mapping(void) {
    Mem mem;
    mem_init(&mem);
    mem.plus = true;
    seed_plus_page(&mem, 0, 0x10);
    seed_plus_page(&mem, 1, 0x11);
    seed_plus_page(&mem, 3, 0x13);
    seed_plus_page(&mem, 5, 0x15);
    mem_plus_reset_mapping(&mem);

    assert(mem_read(&mem, 0x0000) == 0x10);
    assert(mem_read(&mem, 0xC000) == 0x11);

    mem_plus_select_upper_rom(&mem, 7);
    assert(mem_read(&mem, 0xC000) == 0x13);
    mem_plus_select_upper_rom(&mem, 0x85);
    assert(mem_read(&mem, 0xC000) == 0x15);

    /* RMR2 A9 maps cartridge page 1 over RAM bank 1 (0x4000). */
    mem_plus_set_rmr2(&mem, 0xA9);
    assert(mem_read(&mem, 0x4000) == 0x11);

    /* Moving the cartridge overlay must expose RAM in bank 0 rather than
     * leaving cartridge page 0 duplicated at 0x0000. Burnin' Rubber maps
     * page 6 at 0x8000 and continues executing RAM below 0x4000. */
    mem.ram[0x0030] = 0x30;
    mem.cartridge.page[0][0x0030] = 0xC0;
    seed_plus_page(&mem, 6, 0x16);
    mem_plus_set_rmr2(&mem, 0xB6);
    assert(mem_read(&mem, 0x0030) == 0x30);
    assert(mem_read(&mem, 0x8030) == 0x16);

    /* RMR2 B8 maps the ASIC register page at 0x4000. */
    mem_plus_set_rmr2(&mem, 0xB8);
    mem_write(&mem, 0x6400, 0x5A);
    assert(mem_read(&mem, 0x6400) == 0x5A);
    assert(mem.ram[0x6400] == 0x00);
}

static void test_plus_dma_ignores_cpu_ram_mapping(void) {
    Mem mem;
    mem_init(&mem);

    /* Mode 4 maps CPU addresses 4000-7FFF to expansion RAM, but ASIC DMA
     * addresses remain physical offsets in the base 64 KB. */
    mem.ram_bank = 4;
    mem.ram[0x4000] = 0x11;
    mem.ram[0x10000] = 0x22;

    assert(mem_read(&mem, 0x4000) == 0x22);
    assert(mem_read_dma(&mem, 0x4000) == 0x11);
}

int main(void) {
    test_absent_upper_rom_falls_back_to_basic();
    test_present_extension_rom_wins();
    test_amsdos_slot_still_wins();
    test_disabled_upper_rom_reads_ram();
    test_plus_cartridge_mapping();
    test_plus_dma_ignores_cpu_ram_mapping();
    return 0;
}

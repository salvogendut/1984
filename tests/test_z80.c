#include "z80.h"

#include <assert.h>
#include <string.h>

int cpc_frame_count;
int g_debug_enabled;

typedef struct {
    u8 mem[65536];
    int ticked_in_step;
    int ticks;
    int ticks_at_write;
    int writes;
    u16 port;
    u8 value;
} TestBus;

static u8 mem_read(void *ctx, u16 addr) {
    return ((TestBus *)ctx)->mem[addr];
}

static void mem_write(void *ctx, u16 addr, u8 value) {
    ((TestBus *)ctx)->mem[addr] = value;
}

static u8 io_read(void *ctx, u16 port) {
    (void)ctx;
    (void)port;
    return 0xFF;
}

static void io_write(void *ctx, u16 port, u8 value) {
    TestBus *test = ctx;
    test->ticks_at_write = test->ticks;
    test->writes++;
    test->port = port;
    test->value = value;
}

static void tick(void *ctx, int cycles) {
    TestBus *test = ctx;
    test->ticks += cycles;
    test->ticked_in_step += cycles;
}

static Z80Bus make_bus(TestBus *test) {
    Z80Bus bus = {
        .mem_read = mem_read,
        .mem_write = mem_write,
        .io_read = io_read,
        .io_write = io_write,
        .tick = tick,
        .ticked_in_step = &test->ticked_in_step,
        .ctx = test,
    };
    return bus;
}

static void test_out_c_r_is_split_12_plus_4(void) {
    TestBus test = {0};
    Z80Bus bus = make_bus(&test);
    Z80 cpu;
    z80_init(&cpu);
    cpu.bc = 0x7F12;
    test.mem[0] = 0xED;
    test.mem[1] = 0x41; /* OUT (C),B */

    assert(z80_step(&cpu, &bus) == 16);
    assert(test.writes == 1);
    assert(test.port == 0x7F12);
    assert(test.value == 0x7F);
    assert(test.ticks_at_write == 12);
    assert(test.ticked_in_step == 12);
}

static void test_outi_is_split_16_plus_4(void) {
    TestBus test = {0};
    Z80Bus bus = make_bus(&test);
    Z80 cpu;
    z80_init(&cpu);
    cpu.bc = 0x027F;
    cpu.hl = 0x1000;
    test.mem[0] = 0xED;
    test.mem[1] = 0xA3; /* OUTI */
    test.mem[0x1000] = 0x55;

    assert(z80_step(&cpu, &bus) == 20);
    assert(test.writes == 1);
    assert(test.port == 0x017F);
    assert(test.value == 0x55);
    assert(test.ticks_at_write == 16);
    assert(test.ticked_in_step == 16);
}

static void test_repeating_otir_keeps_repeat_cycles(void) {
    TestBus test = {0};
    Z80Bus bus = make_bus(&test);
    Z80 cpu;
    z80_init(&cpu);
    cpu.bc = 0x027F;
    cpu.hl = 0x1000;
    test.mem[0] = 0xED;
    test.mem[1] = 0xB3; /* OTIR */
    test.mem[0x1000] = 0xAA;

    assert(z80_step(&cpu, &bus) == 25);
    assert(cpu.pc == 0);
    assert(test.writes == 1);
    assert(test.port == 0x017F);
    assert(test.value == 0xAA);
    assert(test.ticks_at_write == 16);
    assert(test.ticked_in_step == 16);
}

static void test_cb_prefix_includes_prefix_cycles(void) {
    TestBus test = {0};
    Z80Bus bus = make_bus(&test);
    Z80 cpu;
    z80_init(&cpu);
    cpu.b = 0x81;
    test.mem[0] = 0xCB;
    test.mem[1] = 0x00; /* RLC B */

    assert(z80_step(&cpu, &bus) == 8);
    assert(cpu.b == 0x03);
    assert(cpu.pc == 2);
}

static void test_fd_register_ops_include_prefix_cycles(void) {
    TestBus test = {0};
    Z80Bus bus = make_bus(&test);
    Z80 cpu;
    z80_init(&cpu);
    cpu.iy = 0x120F;
    test.mem[0] = 0xFD;
    test.mem[1] = 0x2C; /* INC IYL */

    assert(z80_step(&cpu, &bus) == 8);
    assert(cpu.iy == 0x1210);
    assert(cpu.pc == 2);
}

static void test_fd_ld_sp_iy_includes_prefix_cycles(void) {
    TestBus test = {0};
    Z80Bus bus = make_bus(&test);
    Z80 cpu;
    z80_init(&cpu);
    cpu.iy = 0x4567;
    test.mem[0] = 0xFD;
    test.mem[1] = 0xF9; /* LD SP,IY */

    assert(z80_step(&cpu, &bus) == 12);
    assert(cpu.sp == 0x4567);
    assert(cpu.pc == 2);
}

static void test_ddcb_prefix_uses_indexed_bit_timing(void) {
    TestBus test = {0};
    Z80Bus bus = make_bus(&test);
    Z80 cpu;
    z80_init(&cpu);
    cpu.ix = 0x2000;
    test.mem[0] = 0xDD;
    test.mem[1] = 0xCB;
    test.mem[2] = 0x05;
    test.mem[3] = 0x46; /* BIT 0,(IX+5) */
    test.mem[0x2005] = 0x01;

    assert(z80_step(&cpu, &bus) == 24);
    assert(cpu.pc == 4);
}

int main(void) {
    test_out_c_r_is_split_12_plus_4();
    test_outi_is_split_16_plus_4();
    test_repeating_otir_keeps_repeat_cycles();
    test_cb_prefix_includes_prefix_cycles();
    test_fd_register_ops_include_prefix_cycles();
    test_fd_ld_sp_iy_includes_prefix_cycles();
    test_ddcb_prefix_uses_indexed_bit_timing();
    return 0;
}

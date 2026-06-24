#include "gate_array.h"

#include <assert.h>
#include <string.h>

static void expect_pixels(u8 mode, u8 value, const u8 expected[8]) {
    u8 actual[8];
    ga_decode_byte(mode, value, actual);
    assert(memcmp(actual, expected, sizeof(actual)) == 0);
}

static void test_mode_decoding(void) {
    static const u8 zero[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    static const u8 mode0_ff[8] = {15, 15, 15, 15, 15, 15, 15, 15};
    static const u8 mode1_88[8] = {3, 3, 0, 0, 0, 0, 0, 0};
    static const u8 mode2_81[8] = {1, 0, 0, 0, 0, 0, 0, 1};
    static const u8 mode3_left1[8] = {1, 1, 1, 1, 0, 0, 0, 0};
    static const u8 mode3_right1[8] = {0, 0, 0, 0, 1, 1, 1, 1};
    static const u8 mode3_left2[8] = {2, 2, 2, 2, 0, 0, 0, 0};
    static const u8 mode3_right2[8] = {0, 0, 0, 0, 2, 2, 2, 2};
    static const u8 mode3_both3[8] = {3, 3, 3, 3, 3, 3, 3, 3};

    expect_pixels(0, 0x00, zero);
    expect_pixels(0, 0xFF, mode0_ff);
    expect_pixels(1, 0x88, mode1_88);
    expect_pixels(2, 0x81, mode2_81);

    expect_pixels(3, 0x80, mode3_left1);
    expect_pixels(3, 0x40, mode3_right1);
    expect_pixels(3, 0x08, mode3_left2);
    expect_pixels(3, 0x04, mode3_right2);
    expect_pixels(3, 0xCC, mode3_both3);
    expect_pixels(3, 0x33, zero);
}

static void test_pen_selection(void) {
    GateArray ga = {0};
    ga_init(&ga);

    ga_write(&ga, 0x0F);
    assert(ga.selected_pen == 15);

    ga_write(&ga, 0x10);
    assert(ga.selected_pen == 16);

    ga_write(&ga, 0x1F);
    assert(ga.selected_pen == 16);
    ga_write(&ga, 0x40 | 0x0B);
    assert(ga.ink[16] == 0x0B);

    ga_write(&ga, 0x20);
    assert(ga.selected_pen == 0);
}

int main(void) {
    test_mode_decoding();
    test_pen_selection();
    return 0;
}

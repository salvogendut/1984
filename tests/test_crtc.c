#include "crtc.h"

#include <assert.h>

static void tick_until_hcc(CRTC *crtc, u16 hcc) {
    while (crtc->hcc != hcc)
        crtc_tick(crtc);
}

static void tick_to_next_line(CRTC *crtc) {
    do {
        crtc_tick(crtc);
    } while (crtc->hcc != 0);
}

static void test_default_type_is_um6845r(void) {
    CRTC crtc;
    crtc_init(&crtc);
    assert(crtc.type == CRTC_TYPE_1);
}

static void test_type_specific_register_readback(void) {
    CRTC crtc;
    crtc_init(&crtc);

    crtc_set_type(&crtc, CRTC_TYPE_0);
    crtc.reg[12] = 0x30;
    crtc.reg[13] = 0x42;
    crtc_select(&crtc, 12);
    assert(crtc_read(&crtc) == 0x30);
    crtc_select(&crtc, 13);
    assert(crtc_read(&crtc) == 0x42);
    crtc_select(&crtc, 0);
    assert(crtc_read(&crtc) == 0);

    crtc_set_type(&crtc, CRTC_TYPE_1);
    crtc_select(&crtc, 12);
    assert(crtc_read(&crtc) == 0);
    crtc.reg[14] = 0x12;
    crtc_select(&crtc, 14);
    assert(crtc_read(&crtc) == 0x12);
    crtc_select(&crtc, 31);
    assert(crtc_read(&crtc) == 0xFF);

    crtc_set_type(&crtc, CRTC_TYPE_2);
    crtc_select(&crtc, 12);
    assert(crtc_read(&crtc) == 0);
    crtc.reg[15] = 0x34;
    crtc_select(&crtc, 15);
    assert(crtc_read(&crtc) == 0x34);

    crtc_set_type(&crtc, CRTC_TYPE_3);
    crtc_select(&crtc, 12);
    assert(crtc_read(&crtc) == 0x30);
}

static void test_type_specific_status_port(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[6] = 10;
    crtc.vcc = 9;
    assert(crtc_read_status(&crtc) == 0x00);
    crtc.vcc = 10;
    assert(crtc_read_status(&crtc) == 0x20);

    crtc_set_type(&crtc, CRTC_TYPE_0);
    assert(crtc_read_status(&crtc) == 0xFF);

    crtc_set_type(&crtc, CRTC_TYPE_3);
    crtc.reg[12] = 0x25;
    crtc_select(&crtc, 12);
    assert(crtc_read_status(&crtc) == 0x25);
}

static void test_register_write_masks(void) {
    CRTC crtc;
    crtc_init(&crtc);

    crtc_select(&crtc, 4);
    crtc_write(&crtc, 0xFF);
    assert(crtc.reg[4] == 0x7F);

    crtc_select(&crtc, 5);
    crtc_write(&crtc, 0xFF);
    assert(crtc.reg[5] == 0x1F);

    crtc_select(&crtc, 12);
    crtc_write(&crtc, 0xFF);
    assert(crtc.reg[12] == 0x3F);

    crtc_select(&crtc, 16);
    crtc_write(&crtc, 0xAA);
    assert(crtc.reg[16] == 0);
}

static void test_r8_mask_depends_on_type(void) {
    CRTC crtc;
    crtc_init(&crtc);

    crtc_set_type(&crtc, CRTC_TYPE_1);
    crtc_select(&crtc, 8);
    crtc_write(&crtc, 0x33);
    assert(crtc.reg[8] == 0x03);

    crtc_set_type(&crtc, CRTC_TYPE_0);
    crtc_write(&crtc, 0x33);
    assert(crtc.reg[8] == 0x33);
}

static void test_type1_r12_r13_reloads_at_scanline_start_while_vcc_zero(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc_set_type(&crtc, CRTC_TYPE_1);
    crtc.reg[0] = 7;
    crtc.vcc = 0;
    crtc.ma = 0x3002;
    crtc.ma_row_start = 0x3000;
    crtc.ma_next_row = 0x3000;

    crtc_select(&crtc, 12);
    crtc_write(&crtc, 0x10);
    crtc_select(&crtc, 13);
    crtc_write(&crtc, 0x20);
    assert(crtc.ma == 0x3002);
    assert(crtc.ma_row_start == 0x3000);

    tick_to_next_line(&crtc);
    assert(crtc.ma == 0x1020);
    assert(crtc.ma_row_start == 0x1020);

    crtc_set_type(&crtc, CRTC_TYPE_0);
    crtc.ma = 0;
    crtc.ma_row_start = 0;
    crtc_select(&crtc, 13);
    crtc_write(&crtc, 0x40);
    assert(crtc.ma == 0);
    assert(crtc.ma_row_start == 0);
}

static void test_type1_collapsed_total_keeps_row_pipeline(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc_set_type(&crtc, CRTC_TYPE_1);
    crtc.reg[0] = 7;
    crtc.reg[4] = 0;
    crtc.reg[9] = 0;
    crtc.reg[12] = 0x10;
    crtc.reg[13] = 0x20;
    crtc.vcc = 0;
    crtc.vlc = 0;
    crtc.hcc = 7;
    crtc.ma_next_row = 0x3040;
    crtc.line_last_raster = false;
    crtc.line_last_frame = false;

    crtc_tick(&crtc);

    assert(crtc.ma_row_start == 0x3040);
    assert(crtc.ma == 0x3040);
}

static void test_hsync_width_zero_depends_on_type(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 7;
    crtc.reg[2] = 3;
    crtc.reg[3] = 0;

    crtc_set_type(&crtc, CRTC_TYPE_1);
    for (int i = 0; i < 16; i++)
        crtc_tick(&crtc);
    assert(!crtc.hsync);

    crtc_init(&crtc);
    crtc.reg[0] = 31;
    crtc.reg[2] = 3;
    crtc.reg[3] = 0;
    crtc_set_type(&crtc, CRTC_TYPE_2);
    tick_until_hcc(&crtc, 3);
    assert(crtc.hsync);
}

static void test_long_hsync_latches_at_cutoff(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[2] = 5;
    crtc.reg[3] = 14;

    tick_until_hcc(&crtc, 5);
    assert(crtc.hsync);
    assert(crtc.hsc == 1);
    assert(!crtc.mode_latch);

    tick_until_hcc(&crtc, 11);
    assert(crtc.hsync);
    assert(crtc.hsc == 7);
    assert(crtc.mode_latch);

    crtc_tick(&crtc);
    assert(!crtc.mode_latch);

    tick_until_hcc(&crtc, 18);
    assert(crtc.hsync);
    assert(crtc.hsc == 14);

    crtc_tick(&crtc);
    assert(crtc.hcc == 19);
    assert(!crtc.hsync);
    assert(!crtc.mode_latch);
}

static void test_short_hsync_latches_at_completion(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[2] = 5;
    crtc.reg[3] = 4;

    tick_until_hcc(&crtc, 8);
    assert(crtc.hsync);
    assert(crtc.hsc == 4);
    assert(!crtc.mode_latch);

    crtc_tick(&crtc);
    assert(crtc.hcc == 9);
    assert(!crtc.hsync);
    assert(crtc.mode_latch);

    crtc_tick(&crtc);
    assert(!crtc.mode_latch);
}

static void test_horizontal_display_enable_is_latched(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 7;
    crtc.reg[1] = 4;

    tick_until_hcc(&crtc, 4);
    assert(!crtc.h_display);
    assert(!crtc.display_enable);

    crtc_select(&crtc, 1);
    crtc_write(&crtc, 6);
    assert(!crtc.h_display);

    tick_until_hcc(&crtc, 0);
    assert(crtc.h_display);
    assert(crtc.display_enable);
}

static void test_horizontal_total_uses_equality_not_threshold(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 10;
    crtc.hcc = 250;

    crtc_tick(&crtc);
    assert(crtc.hcc == 251);
    assert(!crtc.new_scanline);

    crtc.hcc = 255;
    crtc_tick(&crtc);
    assert(crtc.hcc == 0);
    assert(!crtc.new_scanline);

    crtc.hcc = 10;
    crtc_tick(&crtc);
    assert(crtc.hcc == 0);
    assert(crtc.new_scanline);

    crtc_select(&crtc, 0);
    crtc_write(&crtc, 0);
    assert(crtc.reg[0] == 0);
}

static void test_vertical_display_enable_is_latched(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 1;
    crtc_select(&crtc, 4);
    crtc_write(&crtc, 1);
    crtc_select(&crtc, 6);
    crtc_write(&crtc, 1);
    crtc_select(&crtc, 9);
    crtc_write(&crtc, 0);

    while (crtc.vcc != 1)
        crtc_tick(&crtc);
    assert(!crtc.v_display);
    assert(!crtc.display_enable);

    crtc_select(&crtc, 6);
    crtc_write(&crtc, 3);
    assert(!crtc.v_display);

    while (crtc.vcc != 0)
        crtc_tick(&crtc);
    assert(crtc.v_display);
    assert(crtc.display_enable);
}

static void test_vertical_counter_wrap_reenables_display_and_reloads_start(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 1;
    crtc.reg[1] = 1;
    crtc.reg[4] = 5;
    crtc.reg[6] = 14;
    crtc.reg[9] = 0;
    crtc.reg[12] = 0x20;
    crtc.reg[13] = 0x40;
    crtc.hcc = 1;
    crtc.vcc = 127;
    crtc.vlc = 0;
    crtc.ma_row_start = 0x1234;
    crtc.ma_next_row = 0x1234;
    crtc.ma = 0x1235;
    crtc.v_display = false;
    crtc.display_enable = false;
    crtc.line_last_raster = true;
    crtc.line_last_frame = false;

    crtc_tick(&crtc);

    assert(crtc.vcc == 0);
    assert(crtc.v_display);
    assert(crtc.display_enable);
    assert(crtc.ma_row_start == 0x2040);
    assert(crtc.ma == 0x2040);
}

static void test_r6_write_can_disable_current_row(void) {
    CRTC crtc;
    crtc_init(&crtc);

    crtc_select(&crtc, 6);
    crtc_write(&crtc, 0);
    assert(!crtc.v_display);
    assert(!crtc.display_enable);
}

static void test_address_pipeline_advances_at_r1(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 7;
    crtc.reg[1] = 4;
    crtc_select(&crtc, 9);
    crtc_write(&crtc, 0);
    u16 start = crtc.ma_row_start;

    tick_to_next_line(&crtc);
    assert(crtc.ma_row_start == (u16)(start + 4));
}

static void test_address_pipeline_captures_current_ma_after_hcc_overflow(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 10;
    crtc.reg[1] = 1;
    crtc_select(&crtc, 9);
    crtc_write(&crtc, 0);
    crtc.ma_row_start = 0x1000;
    crtc.ma_next_row = 0x1000;
    crtc.ma = 0x10FF;
    crtc.hcc = 255;

    crtc_tick(&crtc);
    assert(crtc.hcc == 0);
    assert(!crtc.new_scanline);

    crtc_tick(&crtc);
    assert(crtc.hcc == 1);
    assert(crtc.ma_next_row == 0x1101);
}

static void test_address_pipeline_repeats_when_r1_is_missed(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 7;
    crtc.reg[1] = 6;
    crtc_select(&crtc, 9);
    crtc_write(&crtc, 0);
    u16 start = crtc.ma_row_start;

    tick_until_hcc(&crtc, 5);
    crtc_select(&crtc, 1);
    crtc_write(&crtc, 4);
    tick_to_next_line(&crtc);
    assert(crtc.ma_row_start == start);
}

static void test_address_pipeline_uses_new_r1_if_ahead(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 7;
    crtc.reg[1] = 4;
    crtc_select(&crtc, 9);
    crtc_write(&crtc, 0);
    u16 start = crtc.ma_row_start;

    tick_until_hcc(&crtc, 2);
    crtc_select(&crtc, 1);
    crtc_write(&crtc, 6);
    tick_to_next_line(&crtc);
    assert(crtc.ma_row_start == (u16)(start + 6));
}

static void test_address_pipeline_waits_for_final_raster(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 7;
    crtc.reg[1] = 4;
    crtc_select(&crtc, 9);
    crtc_write(&crtc, 1);
    u16 start = crtc.ma_row_start;

    tick_to_next_line(&crtc);
    assert(crtc.vlc == 1);
    assert(crtc.ma_row_start == start);

    tick_to_next_line(&crtc);
    assert(crtc.vlc == 0);
    assert(crtc.ma_row_start == (u16)(start + 4));
}

static void test_r9_write_matching_current_raster_resets_current_line(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 7;
    crtc.reg[1] = 4;
    crtc_select(&crtc, 9);
    crtc_write(&crtc, 1);
    crtc.hcc = 3;

    crtc_select(&crtc, 9);
    crtc_write(&crtc, 0);
    tick_to_next_line(&crtc);

    assert(crtc.vlc == 0);
}

static void test_r9_write_below_current_raster_does_not_reset_current_line(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 7;
    crtc.reg[1] = 4;
    crtc.vlc = 2;
    crtc.line_last_raster = false;
    crtc.line_last_frame = false;
    crtc.hcc = 3;

    crtc_select(&crtc, 9);
    crtc_write(&crtc, 1);
    tick_to_next_line(&crtc);

    assert(crtc.vlc == 3);
}

static void test_r9_write_before_c0_two_can_reset_current_line(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 7;
    crtc.reg[1] = 4;
    crtc.vcc = 3;
    crtc_select(&crtc, 9);
    crtc_write(&crtc, 1);
    crtc.hcc = 1;

    crtc_select(&crtc, 9);
    crtc_write(&crtc, 0);
    tick_to_next_line(&crtc);

    assert(crtc.vlc == 0);
    assert(crtc.vcc == 4);
}

static void test_r9_write_matching_current_row_arms_current_frame(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 7;
    crtc.reg[1] = 4;
    crtc.vcc = 3;
    crtc.reg[4] = 3;
    crtc_select(&crtc, 9);
    crtc_write(&crtc, 1);
    crtc.hcc = 3;

    crtc_select(&crtc, 9);
    crtc_write(&crtc, 0);
    tick_to_next_line(&crtc);

    assert(crtc.vcc == 0);
}

static void test_r4_write_matching_current_row_does_not_rearm_current_frame(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 7;
    crtc.reg[1] = 4;
    crtc.vcc = 3;
    crtc.reg[4] = 5;
    crtc_select(&crtc, 9);
    crtc_write(&crtc, 0);
    assert(crtc.line_last_raster);
    assert(!crtc.line_last_frame);

    crtc_select(&crtc, 4);
    crtc_write(&crtc, 5);
    crtc.hcc = 3;

    crtc_select(&crtc, 4);
    crtc_write(&crtc, 3);
    tick_to_next_line(&crtc);

    assert(crtc.vcc == 4);
}

static void test_r4_zero_write_arms_collapsed_frame_window(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 7;
    crtc.reg[1] = 4;
    crtc.reg[4] = 15;
    crtc.reg[9] = 0;
    crtc.vcc = 0;
    crtc.vlc = 0;
    crtc.line_last_raster = true;
    crtc.line_last_frame = false;
    crtc.hcc = 3;

    crtc_select(&crtc, 4);
    crtc_write(&crtc, 0);
    assert(crtc.line_last_frame);

    tick_to_next_line(&crtc);

    assert(crtc.vcc == 0);
}

static void test_r7_line_start_uses_previous_line_length(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 1;
    crtc.reg[7] = 1;
    crtc.vcc = 0;
    crtc.line_last_raster = true;
    crtc.line_last_frame = false;
    crtc.hcc = 1;

    crtc_tick(&crtc);

    assert(crtc.hcc == 0);
    assert(crtc.vcc == 1);
    assert(crtc.last_hend == 1);
    assert(crtc.r7_match);
    assert(!crtc.vsync);

    crtc_init(&crtc);
    crtc.reg[0] = 2;
    crtc.reg[7] = 1;
    crtc.vcc = 0;
    crtc.line_last_raster = true;
    crtc.line_last_frame = false;
    crtc.hcc = 2;

    crtc_tick(&crtc);

    assert(crtc.hcc == 0);
    assert(crtc.vcc == 1);
    assert(crtc.last_hend == 2);
    assert(crtc.r7_match);
    assert(crtc.vsync);
}

static void test_r7_write_before_c0_two_does_not_start_vsync_later_this_row(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 7;
    crtc.reg[7] = 3;
    crtc.vcc = 3;
    crtc.hcc = 1;
    crtc.r7_match = false;
    crtc.vsync = false;

    crtc_select(&crtc, 7);
    crtc_write(&crtc, 3);

    assert(crtc.r7_match);
    assert(!crtc.vsync);

    crtc_tick(&crtc);

    assert(crtc.hcc == 2);
    assert(!crtc.vsync);
}

static void test_r7_write_after_c0_one_can_start_vsync(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[7] = 3;
    crtc.vcc = 3;
    crtc.hcc = 2;
    crtc.r7_match = false;
    crtc.vsync = false;

    crtc_select(&crtc, 7);
    crtc_write(&crtc, 3);

    assert(crtc.r7_match);
    assert(crtc.vsync);
    assert(crtc.vsc == 0);
}

static void test_r7_match_does_not_retrigger_until_vcc_changes(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 7;
    crtc.reg[7] = 3;
    crtc.reg[9] = 3;
    crtc.vcc = 3;
    crtc.vlc = 1;
    crtc.vsync = true;
    crtc.vsc = 15;
    crtc.r7_match = true;
    crtc.line_last_raster = false;
    crtc.line_last_frame = false;
    crtc.hcc = 7;

    crtc_tick(&crtc);

    assert(crtc.hcc == 0);
    assert(crtc.vcc == 3);
    assert(crtc.vlc == 2);
    assert(crtc.r7_match);
    assert(!crtc.vsync);
}

int main(void) {
    test_default_type_is_um6845r();
    test_type_specific_register_readback();
    test_type_specific_status_port();
    test_register_write_masks();
    test_r8_mask_depends_on_type();
    test_type1_r12_r13_reloads_at_scanline_start_while_vcc_zero();
    test_type1_collapsed_total_keeps_row_pipeline();
    test_hsync_width_zero_depends_on_type();
    test_long_hsync_latches_at_cutoff();
    test_short_hsync_latches_at_completion();
    test_horizontal_display_enable_is_latched();
    test_horizontal_total_uses_equality_not_threshold();
    test_vertical_display_enable_is_latched();
    test_vertical_counter_wrap_reenables_display_and_reloads_start();
    test_r6_write_can_disable_current_row();
    test_address_pipeline_advances_at_r1();
    test_address_pipeline_captures_current_ma_after_hcc_overflow();
    test_address_pipeline_repeats_when_r1_is_missed();
    test_address_pipeline_uses_new_r1_if_ahead();
    test_address_pipeline_waits_for_final_raster();
    test_r9_write_matching_current_raster_resets_current_line();
    test_r9_write_below_current_raster_does_not_reset_current_line();
    test_r9_write_before_c0_two_can_reset_current_line();
    test_r9_write_matching_current_row_arms_current_frame();
    test_r4_write_matching_current_row_does_not_rearm_current_frame();
    test_r4_zero_write_arms_collapsed_frame_window();
    test_r7_line_start_uses_previous_line_length();
    test_r7_write_before_c0_two_does_not_start_vsync_later_this_row();
    test_r7_write_after_c0_one_can_start_vsync();
    test_r7_match_does_not_retrigger_until_vcc_changes();
    return 0;
}

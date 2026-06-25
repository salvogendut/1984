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

static void test_vertical_display_enable_is_latched(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 1;
    crtc.reg[4] = 1;
    crtc.reg[6] = 1;
    crtc.reg[9] = 0;

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
    crtc.reg[9] = 0;
    u16 start = crtc.ma_row_start;

    tick_to_next_line(&crtc);
    assert(crtc.ma_row_start == (u16)(start + 4));
}

static void test_address_pipeline_repeats_when_r1_is_missed(void) {
    CRTC crtc;
    crtc_init(&crtc);
    crtc.reg[0] = 7;
    crtc.reg[1] = 6;
    crtc.reg[9] = 0;
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
    crtc.reg[9] = 0;
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
    crtc.reg[9] = 1;
    u16 start = crtc.ma_row_start;

    tick_to_next_line(&crtc);
    assert(crtc.vlc == 1);
    assert(crtc.ma_row_start == start);

    tick_to_next_line(&crtc);
    assert(crtc.vlc == 0);
    assert(crtc.ma_row_start == (u16)(start + 4));
}

int main(void) {
    test_long_hsync_latches_at_cutoff();
    test_short_hsync_latches_at_completion();
    test_horizontal_display_enable_is_latched();
    test_vertical_display_enable_is_latched();
    test_r6_write_can_disable_current_row();
    test_address_pipeline_advances_at_r1();
    test_address_pipeline_repeats_when_r1_is_missed();
    test_address_pipeline_uses_new_r1_if_ahead();
    test_address_pipeline_waits_for_final_raster();
    return 0;
}

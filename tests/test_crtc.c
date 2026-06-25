#include "crtc.h"

#include <assert.h>

static void tick_until_hcc(CRTC *crtc, u16 hcc) {
    while (crtc->hcc != hcc)
        crtc_tick(crtc);
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

int main(void) {
    test_long_hsync_latches_at_cutoff();
    test_short_hsync_latches_at_completion();
    return 0;
}

#include "fdc.h"
#include "leds.h"

#include <assert.h>

void leds_ping(LedId id) {
    (void)id;
}

static void test_sense_interrupt_without_pending_seek(void) {
    FDC fdc;
    fdc_init(&fdc, NULL, NULL);

    fdc_write_data(&fdc, 0x08);

    assert(fdc.phase == FDC_PHASE_RESULT);
    assert(fdc.result_len == 1);
    assert(fdc_read_data(&fdc) == FDC_ST0_IC_INV);
    assert(fdc.phase == FDC_PHASE_CMD);
    assert(fdc_read_status(&fdc) == FDC_MSR_RQM);
}

static void test_sense_interrupt_after_recalibrate(void) {
    FDC fdc;
    fdc_init(&fdc, NULL, NULL);

    fdc_write_data(&fdc, 0x07);
    fdc_write_data(&fdc, 0x01);
    assert(fdc.seek_done);

    fdc_write_data(&fdc, 0x08);

    assert(fdc.phase == FDC_PHASE_RESULT);
    assert(fdc.result_len == 2);
    assert(fdc_read_data(&fdc) == (FDC_ST0_SE | 0x01));
    assert(fdc.phase == FDC_PHASE_RESULT);
    assert(fdc_read_data(&fdc) == 0x00);
    assert(fdc.phase == FDC_PHASE_CMD);
}

int main(void) {
    test_sense_interrupt_without_pending_seek();
    test_sense_interrupt_after_recalibrate();
    return 0;
}

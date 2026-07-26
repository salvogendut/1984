#include <stdio.h>
#include "tape.h"

static int failures;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static void test_dc_offset_is_removed(void) {
    TapeSignalFilter f;
    tape_signal_init(&f);
    for (int i = 0; i < 5000; i++)
        CHECK(tape_signal_sample(&f, 12000, 100) == 0x00);
    CHECK(tape_signal_peak_percent(&f) == 0);
}

static void test_full_level_edges(void) {
    TapeSignalFilter f;
    tape_signal_init(&f);
    CHECK(tape_signal_sample(&f, 0, 100) == 0x00);
    CHECK(tape_signal_sample(&f, 3000, 100) == 0x80);
    CHECK(tape_signal_sample(&f, -3000, 100) == 0x00);
    CHECK(tape_signal_peak_percent(&f) > 0);
    CHECK(tape_signal_peak_percent(&f) <= 100);
}

static void test_noise_rejection_and_gain(void) {
    TapeSignalFilter f;
    tape_signal_init(&f);
    CHECK(tape_signal_sample(&f, 0, 100) == 0x00);
    for (int i = 0; i < 100; i++) {
        s16 sample = (i & 1) ? 100 : -100;
        CHECK(tape_signal_sample(&f, sample, 100) == 0x00);
    }

    tape_signal_init(&f);
    CHECK(tape_signal_sample(&f, 0, 400) == 0x00);
    CHECK(tape_signal_sample(&f, 300, 400) == 0x80);
    CHECK(tape_signal_sample(&f, -300, 400) == 0x00);
}

int main(void) {
    test_dc_offset_is_removed();
    test_full_level_edges();
    test_noise_rejection_and_gain();

    if (failures) {
        fprintf(stderr, "%d tape signal test(s) failed\n", failures);
        return 1;
    }
    puts("tape signal tests passed");
    return 0;
}

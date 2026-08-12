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
    CHECK(tape_signal_pcm(&f) == 0);
}

static void test_full_level_edges(void) {
    TapeSignalFilter f;
    tape_signal_init(&f);
    CHECK(tape_signal_sample(&f, 0, 100) == 0x00);
    CHECK(tape_signal_sample(&f, 3000, 100) == 0x80);
    CHECK(tape_signal_pcm(&f) > 2900);
    CHECK(tape_signal_sample(&f, -3000, 100) == 0x00);
    CHECK(tape_signal_pcm(&f) < -2900);
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
    CHECK(tape_signal_pcm(&f) > 1100);
    CHECK(tape_signal_sample(&f, -300, 400) == 0x00);
    CHECK(tape_signal_pcm(&f) < -1100);
}

static void test_virtual_deck_transport(void) {
    static const char path[] = "/tmp/1984-test-tape-transport.cdt";
    static const u8 image[] = {
        'Z', 'X', 'T', 'a', 'p', 'e', '!', 0x1a, 1, 20,
        0x12, 0xe8, 0x03, 0xa0, 0x0f /* 1000 T-state tone, 4000 pulses */
    };
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    if (!file) return;
    CHECK(fwrite(image, 1, sizeof(image), file) == sizeof(image));
    fclose(file);

    Tape tape;
    tape_init(&tape);
    CHECK(tape_load(&tape, path));
    tape_set_motor(&tape, true);
    tape_set_paused(&tape, true);
    CHECK(!tape_playing(&tape));
    tape_step(&tape, 4000000);
    CHECK(tape_counter_seconds(&tape) == 0);

    tape_set_paused(&tape, false);
    CHECK(tape_playing(&tape));
    tape_step(&tape, 4000000);
    CHECK(tape_counter_seconds(&tape) == 1);
    tape_rewind(&tape);
    CHECK(tape_counter_seconds(&tape) == 0);
    tape_next_block(&tape);
    CHECK(tape.stage == TAPE_END);
    tape_eject(&tape);
    remove(path);
}

int main(void) {
    test_dc_offset_is_removed();
    test_full_level_edges();
    test_noise_rejection_and_gain();
    test_virtual_deck_transport();

    if (failures) {
        fprintf(stderr, "%d tape signal test(s) failed\n", failures);
        return 1;
    }
    puts("tape signal tests passed");
    return 0;
}

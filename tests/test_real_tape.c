#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../src/real_tape.h"

static uint32_t read_le32(const unsigned char *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int16_t read_le16(const unsigned char *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void test_modes_and_targets(void) {
    RealTapeMode mode;
    RealTapeOutputTarget target;

    assert(real_tape_mode_parse("input", &mode));
    assert(mode == REAL_TAPE_INPUT);
    assert(real_tape_mode_parse("OUTPUT", &mode));
    assert(mode == REAL_TAPE_OUTPUT);
    assert(real_tape_mode_parse("load", &mode));
    assert(mode == REAL_TAPE_INPUT);
    assert(real_tape_mode_parse("save", &mode));
    assert(mode == REAL_TAPE_OUTPUT);
    assert(real_tape_mode_parse("both", &mode));
    assert(mode == REAL_TAPE_INPUT);
    assert(!real_tape_mode_parse("invalid", &mode));

    assert(real_tape_output_target_parse("file", &target));
    assert(target == REAL_TAPE_TARGET_FILE);
    assert(real_tape_output_target_parse("DEVICE", &target));
    assert(target == REAL_TAPE_TARGET_DEVICE);
    assert(!real_tape_output_target_parse("both", &target));
}

static void test_output_wav(void) {
    static const char path[] = "/tmp/1984-test-real-tape.wav";
    unsigned char wav[48];
    RealTape rt;

    real_tape_init(&rt);
    assert(real_tape_configure(&rt, REAL_TAPE_OUTPUT, "default",
                               REAL_TAPE_TARGET_FILE, "default",
                               100, 50, true, true));
    assert(real_tape_record_start(&rt, path));
    real_tape_output_sample(&rt, 0x80);
    real_tape_output_sample(&rt, 0x00);
    real_tape_record_stop(&rt);

    FILE *f = fopen(path, "rb");
    assert(f);
    assert(fread(wav, 1, sizeof(wav), f) == sizeof(wav));
    assert(fgetc(f) == EOF);
    fclose(f);

    assert(!memcmp(wav, "RIFF", 4));
    assert(read_le32(wav + 4) == 40);
    assert(!memcmp(wav + 8, "WAVE", 4));
    assert(read_le32(wav + 40) == 4);
    assert(read_le16(wav + 44) == 16383);
    assert(read_le16(wav + 46) == -16383);

    remove(path);
    real_tape_shutdown(&rt);
}

static void test_output_device(void) {
    RealTape rt;

    assert(SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"));
    assert(SDL_Init(SDL_INIT_AUDIO));
    real_tape_init(&rt);
    assert(real_tape_configure(&rt, REAL_TAPE_OUTPUT, "default",
                               REAL_TAPE_TARGET_DEVICE, "default",
                               100, 50, true, true));
    assert(rt.output_stream);
    real_tape_output_sample(&rt, 0x80);
    real_tape_output_sample(&rt, 0x00);
    real_tape_flush_output(&rt);
    assert(!real_tape_error(&rt)[0]);
    assert(!real_tape_record_start(&rt, "/tmp/should-not-exist.wav"));
    real_tape_shutdown(&rt);
    SDL_Quit();
}

int main(void) {
    test_modes_and_targets();
    test_output_wav();
    test_output_device();
    puts("real tape tests passed");
    return 0;
}

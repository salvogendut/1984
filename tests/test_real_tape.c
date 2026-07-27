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

static uint32_t read_le24(const unsigned char *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16);
}

static int16_t read_le16(const unsigned char *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void write_test_wav(const char *path) {
    RealTape rt;

    real_tape_init(&rt);
    assert(real_tape_configure(&rt, REAL_TAPE_OUTPUT, "default",
                               REAL_TAPE_OUTPUT_SOURCE_CDT,
                               REAL_TAPE_TARGET_FILE, "default",
                               100, 50, true, true));
    assert(real_tape_record_start(&rt, path));
    real_tape_output_sample(&rt, 0x80);
    real_tape_output_sample(&rt, 0x00);
    real_tape_record_stop(&rt);
    real_tape_shutdown(&rt);
}

static void test_modes_and_targets(void) {
    RealTapeMode mode;
    RealTapeOutputSource source;
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

    assert(real_tape_output_source_parse("cdt", &source));
    assert(source == REAL_TAPE_OUTPUT_SOURCE_CDT);
    assert(real_tape_output_source_parse("CPC_SAVE", &source));
    assert(source == REAL_TAPE_OUTPUT_SOURCE_CPC_SAVE);
    assert(real_tape_output_source_parse("cpc", &source));
    assert(source == REAL_TAPE_OUTPUT_SOURCE_CPC_SAVE);
    assert(!real_tape_output_source_parse("system", &source));

    assert(real_tape_output_target_parse("file", &target));
    assert(target == REAL_TAPE_TARGET_FILE);
    assert(real_tape_output_target_parse("DEVICE", &target));
    assert(target == REAL_TAPE_TARGET_DEVICE);
    assert(!real_tape_output_target_parse("both", &target));

    char path[64] = "/tmp/converted";
    assert(real_tape_ensure_cdt_extension(path, sizeof(path)));
    assert(!strcmp(path, "/tmp/converted.cdt"));
}

static void test_output_wav(void) {
    static const char path[] = "/tmp/1984-test-real-tape.wav";
    unsigned char wav[48];

    write_test_wav(path);

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
}

static void test_cpc_save_output_wav(void) {
    static const char path[] = "/tmp/1984-test-cpc-save.wav";
    unsigned char wav[48];
    int16_t waveform[2];
    RealTape rt;

    real_tape_init(&rt);
    assert(real_tape_configure(
        &rt, REAL_TAPE_OUTPUT, "default",
        REAL_TAPE_OUTPUT_SOURCE_CPC_SAVE,
        REAL_TAPE_TARGET_FILE, "default",
        100, 50, true, true));
    assert(real_tape_record_start(&rt, path));

    real_tape_sample(&rt, 0x20); /* write HIGH, motor off */
    assert(real_tape_waveform_copy(&rt, waveform, 2) == 0);
    real_tape_sample(&rt, 0x30); /* write HIGH, motor on */
    real_tape_sample(&rt, 0x10); /* write LOW, motor on */
    assert(real_tape_waveform_copy(&rt, waveform, 2) == 2);
    assert(waveform[0] == 16383);
    assert(waveform[1] == -16383);

    real_tape_record_stop(&rt);
    FILE *f = fopen(path, "rb");
    assert(f);
    assert(fread(wav, 1, sizeof(wav), f) == sizeof(wav));
    assert(fgetc(f) == EOF);
    fclose(f);
    assert(read_le32(wav + 40) == 4);
    assert(read_le16(wav + 44) == 16383);
    assert(read_le16(wav + 46) == -16383);

    real_tape_shutdown(&rt);
    remove(path);
}

static void test_input_wav_monitors(void) {
    static const char path[] = "/tmp/1984-test-real-tape-input.wav";
    int16_t samples[2];
    RealTape rt;

    write_test_wav(path);
    real_tape_init(&rt);
    assert(real_tape_source_load_wav(&rt, path));
    assert(real_tape_configure(&rt, REAL_TAPE_INPUT, "default",
                               REAL_TAPE_OUTPUT_SOURCE_CDT,
                               REAL_TAPE_TARGET_FILE, "default",
                               100, 50, true, true));
    assert(!rt.input_stream);
    assert(real_tape_source_remaining_seconds(&rt) == 1);

    real_tape_sample(&rt, 0x10);
    real_tape_sample(&rt, 0x10);
    assert(real_tape_source_remaining_seconds(&rt) == 0);
    assert(real_tape_monitor_sample(&rt) != 0);
    assert(real_tape_waveform_copy(&rt, samples, 2) == 2);
    assert(samples[1] != 0);

    assert(real_tape_configure(&rt, REAL_TAPE_INPUT, "default",
                               REAL_TAPE_OUTPUT_SOURCE_CDT,
                               REAL_TAPE_TARGET_FILE, "default",
                               100, 50, false, false));
    real_tape_reset(&rt);
    real_tape_sample(&rt, 0x10);
    real_tape_sample(&rt, 0x10);
    assert(real_tape_monitor_sample(&rt) == 0);
    assert(real_tape_waveform_copy(&rt, samples, 2) == 0);

    real_tape_shutdown(&rt);
    remove(path);
}

static void test_input_wav_to_cdt(void) {
    static const char wav_path[] = "/tmp/1984-test-cdt-input.wav";
    static const char cdt_path[] = "/tmp/1984-test-cdt-output.cdt";
    unsigned char cdt[20];
    RealTape rt;
    Tape tape;

    write_test_wav(wav_path);
    real_tape_init(&rt);
    assert(real_tape_source_load_wav(&rt, wav_path));
    rt.source_samples[0] = 0;
    rt.source_samples[1] = 32767;
    assert(real_tape_configure(&rt, REAL_TAPE_INPUT, "default",
                               REAL_TAPE_OUTPUT_SOURCE_CDT,
                               REAL_TAPE_TARGET_FILE, "default",
                               100, 50, true, true));
    assert(!real_tape_record_start(&rt, "/tmp/wrong-extension.wav"));
    assert(real_tape_record_start(&rt, cdt_path));
    assert(rt.capture_format == REAL_TAPE_CAPTURE_CDT);

    real_tape_sample(&rt, 0x10);
    real_tape_sample(&rt, 0x10);
    assert(!real_tape_recording(&rt));

    FILE *f = fopen(cdt_path, "rb");
    assert(f);
    assert(fread(cdt, 1, sizeof(cdt), f) == sizeof(cdt));
    assert(fgetc(f) == EOF);
    fclose(f);

    assert(!memcmp(cdt, "ZXTape!\x1A", 8));
    assert(cdt[8] == 1 && cdt[9] == 20);
    assert(cdt[10] == 0x15);
    assert((uint16_t)read_le16(cdt + 11) == 79);
    assert(cdt[15] == 2);
    assert(read_le24(cdt + 16) == 1);
    assert(cdt[19] == 0x40);

    tape_init(&tape);
    assert(tape_load(&tape, cdt_path));
    assert(tape.stage == TAPE_DIRECT);
    assert(tape_level(&tape) == 0x00);
    tape_set_motor(&tape, true);
    tape_step(&tape, 90);
    assert(tape.stage == TAPE_DIRECT);
    assert(tape_level(&tape) == 0x80);
    tape_step(&tape, 90);
    assert(tape.stage == TAPE_END);
    tape_eject(&tape);

    real_tape_shutdown(&rt);
    remove(wav_path);
    remove(cdt_path);
}

static void test_output_device(void) {
    RealTape rt;

    assert(SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"));
    assert(SDL_Init(SDL_INIT_AUDIO));
    real_tape_init(&rt);
    assert(real_tape_configure(&rt, REAL_TAPE_OUTPUT, "default",
                               REAL_TAPE_OUTPUT_SOURCE_CDT,
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

static void test_input_wav_output_device(void) {
    static const char path[] = "/tmp/1984-test-device-input.wav";
    RealTape rt;

    assert(SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"));
    assert(SDL_Init(SDL_INIT_AUDIO));
    write_test_wav(path);

    real_tape_init(&rt);
    assert(real_tape_source_load_wav(&rt, path));
    assert(real_tape_configure(&rt, REAL_TAPE_INPUT, "default",
                               REAL_TAPE_OUTPUT_SOURCE_CDT,
                               REAL_TAPE_TARGET_DEVICE, "default",
                               100, 50, true, true));
    assert(rt.output_stream);
    real_tape_sample(&rt, 0x10);
    assert(rt.output_frame_count == 1);
    assert(rt.output_frame[0] == 8191);
    real_tape_flush_output(&rt);
    assert(rt.output_frame_count == 0);
    assert(!real_tape_error(&rt)[0]);

    real_tape_shutdown(&rt);
    remove(path);
    SDL_Quit();
}

static void test_output_monitors(void) {
    RealTape rt;
    int16_t samples[2];

    real_tape_init(&rt);
    assert(real_tape_configure(&rt, REAL_TAPE_OUTPUT, "default",
                               REAL_TAPE_OUTPUT_SOURCE_CDT,
                               REAL_TAPE_TARGET_FILE, "default",
                               100, 50, true, true));
    assert(!rt.input_stream);
    assert(real_tape_audible_monitor_enabled(&rt));
    assert(real_tape_visual_monitor_enabled(&rt));

    real_tape_output_sample(&rt, 0x80);
    real_tape_output_sample(&rt, 0x00);
    assert(real_tape_waveform_copy(&rt, samples, 2) == 2);
    assert(samples[0] == 16383);
    assert(samples[1] == -16383);

    assert(real_tape_configure(&rt, REAL_TAPE_OUTPUT, "default",
                               REAL_TAPE_OUTPUT_SOURCE_CDT,
                               REAL_TAPE_TARGET_FILE, "default",
                               100, 50, false, false));
    assert(!real_tape_audible_monitor_enabled(&rt));
    assert(!real_tape_visual_monitor_enabled(&rt));
    assert(real_tape_waveform_copy(&rt, samples, 2) == 0);
    real_tape_output_sample(&rt, 0x80);
    assert(real_tape_waveform_copy(&rt, samples, 2) == 0);
    real_tape_shutdown(&rt);
}

int main(void) {
    test_modes_and_targets();
    test_output_wav();
    test_cpc_save_output_wav();
    test_input_wav_monitors();
    test_input_wav_to_cdt();
    test_output_device();
    test_input_wav_output_device();
    test_output_monitors();
    puts("real tape tests passed");
    return 0;
}

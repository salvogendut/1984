#pragma once

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include "tape.h"
#include "types.h"

#define REAL_TAPE_DEVICE_NAME_MAX 256
#define REAL_TAPE_PATH_MAX 512
#define REAL_TAPE_SAMPLE_RATE 44100
#define REAL_TAPE_WAVEFORM_SAMPLES 4096
#define REAL_TAPE_INPUT_GAIN_DEFAULT 100
#define REAL_TAPE_OUTPUT_LEVEL_DEFAULT 50

typedef enum {
    REAL_TAPE_OFF = 0,
    REAL_TAPE_INPUT,
    REAL_TAPE_OUTPUT,
} RealTapeMode;

typedef enum {
    REAL_TAPE_OUTPUT_SOURCE_CDT = 0,
    REAL_TAPE_OUTPUT_SOURCE_CPC_SAVE,
} RealTapeOutputSource;

typedef enum {
    REAL_TAPE_TARGET_FILE = 0,
    REAL_TAPE_TARGET_DEVICE,
} RealTapeOutputTarget;

typedef enum {
    REAL_TAPE_CAPTURE_NONE = 0,
    REAL_TAPE_CAPTURE_WAV,
    REAL_TAPE_CAPTURE_CDT,
} RealTapeCaptureFormat;

typedef struct {
    RealTapeMode mode;
    RealTapeOutputSource output_source;
    RealTapeOutputTarget output_target;
    SDL_AudioStream *input_stream;
    SDL_AudioStream *output_stream;
    char input_device[REAL_TAPE_DEVICE_NAME_MAX];
    char output_device[REAL_TAPE_DEVICE_NAME_MAX];
    char active_input[REAL_TAPE_DEVICE_NAME_MAX];
    char active_output[REAL_TAPE_DEVICE_NAME_MAX];
    int input_gain;
    int output_level;
    bool audible_monitor;
    bool visual_monitor;

    s16 input_ring[4096];
    size_t input_head;
    size_t input_count;
    unsigned input_underruns;
    unsigned input_overruns;
    TapeSignalFilter input_filter;
    u8 input_level;
    s16 monitor_pcm;
    s16 waveform_ring[REAL_TAPE_WAVEFORM_SAMPLES];
    size_t waveform_head;
    size_t waveform_count;

    s16 *source_samples;
    size_t source_sample_count;
    size_t source_sample_pos;
    char source_path[REAL_TAPE_PATH_MAX];

    s16 output_frame[8192];
    int output_frame_count;

    FILE *capture;
    RealTapeCaptureFormat capture_format;
    char capture_path[REAL_TAPE_PATH_MAX];
    u32 capture_bytes;
    u8 cdt_byte;
    u8 cdt_bits;
    bool cdt_block_started;

    char error[256];
} RealTape;

void real_tape_init(RealTape *rt);
void real_tape_shutdown(RealTape *rt);
void real_tape_reset(RealTape *rt);

bool real_tape_configure(RealTape *rt, RealTapeMode mode,
                         const char *input_device,
                         RealTapeOutputSource output_source,
                         RealTapeOutputTarget output_target,
                         const char *output_device,
                         int input_gain, int output_level,
                         bool audible_monitor, bool visual_monitor);
void real_tape_pump(RealTape *rt);
void real_tape_sample(RealTape *rt, u8 ppi_port_c);
void real_tape_output_sample(RealTape *rt, u8 tape_level);
void real_tape_flush_output(RealTape *rt);

bool real_tape_input_active(const RealTape *rt);
bool real_tape_connected_input_active(const RealTape *rt);
bool real_tape_audible_monitor_enabled(const RealTape *rt);
bool real_tape_visual_monitor_enabled(const RealTape *rt);
u8 real_tape_input_level(const RealTape *rt);
int real_tape_signal_percent(const RealTape *rt);
int real_tape_buffered_ms(const RealTape *rt);
const char *real_tape_error(const RealTape *rt);
s16 real_tape_monitor_sample(const RealTape *rt);
size_t real_tape_waveform_copy(const RealTape *rt, s16 *samples,
                               size_t capacity);

bool real_tape_source_load_wav(RealTape *rt, const char *path);
void real_tape_source_eject(RealTape *rt);
bool real_tape_source_loaded(const RealTape *rt);
const char *real_tape_source_path(const RealTape *rt);
int real_tape_source_progress(const RealTape *rt);
u32 real_tape_source_remaining_seconds(const RealTape *rt);

const char *real_tape_mode_name(RealTapeMode mode);
bool real_tape_mode_parse(const char *text, RealTapeMode *mode);
bool real_tape_mode_has_input(RealTapeMode mode);
bool real_tape_mode_has_output(RealTapeMode mode);
const char *real_tape_output_source_name(RealTapeOutputSource source);
bool real_tape_output_source_parse(const char *text,
                                   RealTapeOutputSource *source);
const char *real_tape_output_target_name(RealTapeOutputTarget target);
bool real_tape_output_target_parse(const char *text,
                                   RealTapeOutputTarget *target);

bool real_tape_cycle_input_device(const char *current, int direction,
                                  char *next, size_t next_size);
bool real_tape_cycle_output_device(const char *current, int direction,
                                   char *next, size_t next_size);
void real_tape_device_label(const char *configured,
                            char *label, size_t label_size);

bool real_tape_record_start(RealTape *rt, const char *path);
void real_tape_record_stop(RealTape *rt);
bool real_tape_recording(const RealTape *rt);
const char *real_tape_record_path(const RealTape *rt);
bool real_tape_ensure_wav_extension(char *path, size_t size);
bool real_tape_ensure_cdt_extension(char *path, size_t size);

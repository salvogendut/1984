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
#define REAL_TAPE_INPUT_GAIN_DEFAULT 100
#define REAL_TAPE_OUTPUT_LEVEL_DEFAULT 50

typedef enum {
    REAL_TAPE_OFF = 0,
    REAL_TAPE_LOAD,
    REAL_TAPE_SAVE,
    REAL_TAPE_BOTH,
} RealTapeMode;

typedef struct {
    RealTapeMode mode;
    SDL_AudioStream *input_stream;
    SDL_AudioStream *output_stream;
    char input_device[REAL_TAPE_DEVICE_NAME_MAX];
    char output_device[REAL_TAPE_DEVICE_NAME_MAX];
    char active_input[REAL_TAPE_DEVICE_NAME_MAX];
    char active_output[REAL_TAPE_DEVICE_NAME_MAX];
    int input_gain;
    int output_level;

    s16 input_ring[4096];
    size_t input_head;
    size_t input_count;
    unsigned input_underruns;
    unsigned input_overruns;
    TapeSignalFilter input_filter;
    u8 input_level;

    s16 output_frame[8192];
    int output_frame_count;

    FILE *wav;
    char wav_path[REAL_TAPE_PATH_MAX];
    u32 wav_bytes;

    char error[256];
} RealTape;

void real_tape_init(RealTape *rt);
void real_tape_shutdown(RealTape *rt);
void real_tape_reset(RealTape *rt);

bool real_tape_configure(RealTape *rt, RealTapeMode mode,
                         const char *input_device,
                         const char *output_device,
                         int input_gain, int output_level);
void real_tape_pump(RealTape *rt);
void real_tape_sample(RealTape *rt, u8 ppi_port_c);
void real_tape_flush_output(RealTape *rt);

bool real_tape_input_active(const RealTape *rt);
bool real_tape_output_active(const RealTape *rt);
u8 real_tape_input_level(const RealTape *rt);
int real_tape_signal_percent(const RealTape *rt);
int real_tape_buffered_ms(const RealTape *rt);
const char *real_tape_error(const RealTape *rt);

const char *real_tape_mode_name(RealTapeMode mode);
bool real_tape_mode_parse(const char *text, RealTapeMode *mode);
bool real_tape_mode_has_input(RealTapeMode mode);
bool real_tape_mode_has_output(RealTapeMode mode);

bool real_tape_cycle_device(bool recording, const char *current, int direction,
                            char *next, size_t next_size);
void real_tape_device_label(const char *configured,
                            char *label, size_t label_size);

bool real_tape_record_start(RealTape *rt, const char *path);
void real_tape_record_stop(RealTape *rt);
bool real_tape_recording(const RealTape *rt);
const char *real_tape_record_path(const RealTape *rt);
bool real_tape_ensure_wav_extension(char *path, size_t size);

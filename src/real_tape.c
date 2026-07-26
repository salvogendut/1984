#include "real_tape.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool default_device(const char *name) {
    return !name || !name[0] || !strcmp(name, "default");
}

bool real_tape_mode_has_input(RealTapeMode mode) {
    return mode == REAL_TAPE_LOAD || mode == REAL_TAPE_BOTH;
}

bool real_tape_mode_has_output(RealTapeMode mode) {
    return mode == REAL_TAPE_SAVE || mode == REAL_TAPE_BOTH;
}

const char *real_tape_mode_name(RealTapeMode mode) {
    switch (mode) {
    case REAL_TAPE_LOAD: return "load";
    case REAL_TAPE_SAVE: return "save";
    case REAL_TAPE_BOTH: return "both";
    default:             return "off";
    }
}

bool real_tape_mode_parse(const char *text, RealTapeMode *mode) {
    if (!text || !mode) return false;
    if (!strcmp(text, "off"))       *mode = REAL_TAPE_OFF;
    else if (!strcmp(text, "load")) *mode = REAL_TAPE_LOAD;
    else if (!strcmp(text, "save")) *mode = REAL_TAPE_SAVE;
    else if (!strcmp(text, "both")) *mode = REAL_TAPE_BOTH;
    else return false;
    return true;
}

static void set_error(RealTape *rt, const char *side, const char *detail) {
    snprintf(rt->error, sizeof(rt->error), "%s: %s",
             side, detail && detail[0] ? detail : "unknown audio error");
    fprintf(stderr, "[real-tape] %s\n", rt->error);
}

static SDL_AudioDeviceID resolve_device(bool recording, const char *name,
                                        char *active, size_t active_size) {
    SDL_AudioDeviceID default_id = recording
        ? SDL_AUDIO_DEVICE_DEFAULT_RECORDING
        : SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (default_device(name)) {
        const char *current = SDL_GetAudioDeviceName(default_id);
        snprintf(active, active_size, "%s",
                 current && current[0] ? current : "System default");
        return default_id;
    }

    int count = 0;
    SDL_AudioDeviceID *devices = recording
        ? SDL_GetAudioRecordingDevices(&count)
        : SDL_GetAudioPlaybackDevices(&count);
    SDL_AudioDeviceID found = 0;
    for (int i = 0; devices && i < count; i++) {
        const char *candidate = SDL_GetAudioDeviceName(devices[i]);
        if (candidate && !strcmp(candidate, name)) {
            found = devices[i];
            snprintf(active, active_size, "%s", candidate);
            break;
        }
    }
    SDL_free(devices);
    return found;
}

static SDL_AudioStream *open_stream(RealTape *rt, bool recording,
                                    const char *name,
                                    char *active, size_t active_size) {
    SDL_AudioDeviceID device = resolve_device(recording, name,
                                               active, active_size);
    if (!device) {
        set_error(rt, recording ? "input device" : "output device",
                  "configured device is not connected");
        return NULL;
    }

    SDL_AudioSpec spec = { SDL_AUDIO_S16, 1, REAL_TAPE_SAMPLE_RATE };
    SDL_AudioStream *stream = SDL_OpenAudioDeviceStream(device, &spec,
                                                        NULL, NULL);
    if (!stream) {
        set_error(rt, recording ? "input device" : "output device",
                  SDL_GetError());
        return NULL;
    }
    if (!SDL_ResumeAudioStreamDevice(stream)) {
        set_error(rt, recording ? "input device" : "output device",
                  SDL_GetError());
        SDL_DestroyAudioStream(stream);
        return NULL;
    }
    return stream;
}

void real_tape_init(RealTape *rt) {
    memset(rt, 0, sizeof(*rt));
    rt->input_gain = REAL_TAPE_INPUT_GAIN_DEFAULT;
    rt->output_level = REAL_TAPE_OUTPUT_LEVEL_DEFAULT;
    snprintf(rt->input_device, sizeof(rt->input_device), "default");
    snprintf(rt->output_device, sizeof(rt->output_device), "default");
    tape_signal_init(&rt->input_filter);
}

void real_tape_record_stop(RealTape *rt) {
    if (!rt || !rt->wav) return;

    if (fseek(rt->wav, 4, SEEK_SET) == 0) {
        u32 riff_size = rt->wav_bytes + 36;
        u8 b[4] = {
            (u8)riff_size, (u8)(riff_size >> 8),
            (u8)(riff_size >> 16), (u8)(riff_size >> 24)
        };
        fwrite(b, 1, sizeof(b), rt->wav);
    }
    if (fseek(rt->wav, 40, SEEK_SET) == 0) {
        u8 b[4] = {
            (u8)rt->wav_bytes, (u8)(rt->wav_bytes >> 8),
            (u8)(rt->wav_bytes >> 16), (u8)(rt->wav_bytes >> 24)
        };
        fwrite(b, 1, sizeof(b), rt->wav);
    }
    fclose(rt->wav);
    rt->wav = NULL;
    fprintf(stderr, "[real-tape] WAV stopped (%u bytes PCM): %s\n",
            rt->wav_bytes, rt->wav_path);
}

void real_tape_shutdown(RealTape *rt) {
    if (!rt) return;
    real_tape_record_stop(rt);
    if (rt->input_stream) SDL_DestroyAudioStream(rt->input_stream);
    if (rt->output_stream) SDL_DestroyAudioStream(rt->output_stream);
    rt->input_stream = NULL;
    rt->output_stream = NULL;
    rt->mode = REAL_TAPE_OFF;
}

void real_tape_reset(RealTape *rt) {
    if (!rt) return;
    rt->input_head = 0;
    rt->input_count = 0;
    rt->output_frame_count = 0;
    rt->input_level = 0;
    tape_signal_init(&rt->input_filter);
    if (rt->input_stream) SDL_ClearAudioStream(rt->input_stream);
    if (rt->output_stream) SDL_ClearAudioStream(rt->output_stream);
}

bool real_tape_configure(RealTape *rt, RealTapeMode mode,
                         const char *input_device,
                         const char *output_device,
                         int input_gain, int output_level) {
    if (!rt) return false;
    const char *input = default_device(input_device) ? "default" : input_device;
    const char *output = default_device(output_device) ? "default" : output_device;
    bool reopen_input = real_tape_mode_has_input(mode) &&
        (!rt->input_stream || strcmp(rt->input_device, input));
    bool reopen_output = real_tape_mode_has_output(mode) &&
        (!rt->output_stream || strcmp(rt->output_device, output));

    rt->error[0] = '\0';
    rt->mode = mode;
    rt->input_gain = input_gain < 25 ? 25 :
                     input_gain > 400 ? 400 : input_gain;
    rt->output_level = output_level < 0 ? 0 :
                       output_level > 100 ? 100 : output_level;

    if (!real_tape_mode_has_input(mode)) {
        real_tape_record_stop(rt);
        if (rt->input_stream) SDL_DestroyAudioStream(rt->input_stream);
        rt->input_stream = NULL;
        rt->active_input[0] = '\0';
        rt->input_head = 0;
        rt->input_count = 0;
        rt->input_level = 0;
        tape_signal_init(&rt->input_filter);
    } else if (reopen_input) {
        if (rt->input_stream) SDL_DestroyAudioStream(rt->input_stream);
        rt->input_stream = NULL;
        rt->input_head = 0;
        rt->input_count = 0;
        tape_signal_init(&rt->input_filter);
        rt->input_stream = open_stream(rt, true, input,
                                       rt->active_input,
                                       sizeof(rt->active_input));
    }

    if (!real_tape_mode_has_output(mode)) {
        if (rt->output_stream) SDL_DestroyAudioStream(rt->output_stream);
        rt->output_stream = NULL;
        rt->active_output[0] = '\0';
        rt->output_frame_count = 0;
    } else if (reopen_output) {
        if (rt->output_stream) SDL_DestroyAudioStream(rt->output_stream);
        rt->output_stream = NULL;
        rt->output_frame_count = 0;
        rt->output_stream = open_stream(rt, false, output,
                                        rt->active_output,
                                        sizeof(rt->active_output));
    }

    snprintf(rt->input_device, sizeof(rt->input_device), "%s", input);
    snprintf(rt->output_device, sizeof(rt->output_device), "%s", output);

    bool input_ok = !real_tape_mode_has_input(mode) || rt->input_stream;
    bool output_ok = !real_tape_mode_has_output(mode) || rt->output_stream;
    return input_ok && output_ok;
}

void real_tape_pump(RealTape *rt) {
    if (!real_tape_input_active(rt)) return;

    s16 samples[2048];
    int available = SDL_GetAudioStreamAvailable(rt->input_stream);
    while (available > 0) {
        int wanted = available;
        if (wanted > (int)sizeof(samples)) wanted = (int)sizeof(samples);
        wanted &= ~(int)(sizeof(s16) - 1);
        int got = SDL_GetAudioStreamData(rt->input_stream, samples, wanted);
        if (got < 0) {
            set_error(rt, "input stream", SDL_GetError());
            return;
        }
        if (got == 0) break;

        size_t count = (size_t)got / sizeof(samples[0]);
        size_t wav_bytes = count * sizeof(samples[0]);
        if (rt->wav && wav_bytes > UINT32_MAX - rt->wav_bytes) {
            set_error(rt, "WAV capture", "file reached the 4 GB WAV limit");
            real_tape_record_stop(rt);
        } else if (rt->wav &&
                   fwrite(samples, sizeof(samples[0]), count, rt->wav) != count) {
            set_error(rt, "WAV capture", "write failed");
            real_tape_record_stop(rt);
        } else if (rt->wav) {
            rt->wav_bytes += (u32)wav_bytes;
        }

        for (size_t i = 0; i < count; i++) {
            if (rt->input_count == SDL_arraysize(rt->input_ring)) {
                rt->input_head =
                    (rt->input_head + 1) % SDL_arraysize(rt->input_ring);
                rt->input_count--;
                rt->input_overruns++;
            }
            size_t tail = (rt->input_head + rt->input_count)
                        % SDL_arraysize(rt->input_ring);
            rt->input_ring[tail] = samples[i];
            rt->input_count++;
        }
        available = SDL_GetAudioStreamAvailable(rt->input_stream);
    }
}

void real_tape_sample(RealTape *rt, u8 ppi_port_c) {
    if (real_tape_input_active(rt)) {
        if (rt->input_count > 0) {
            s16 sample = rt->input_ring[rt->input_head];
            rt->input_head =
                (rt->input_head + 1) % SDL_arraysize(rt->input_ring);
            rt->input_count--;
            rt->input_level = tape_signal_sample(&rt->input_filter, sample,
                                                  rt->input_gain);
        } else {
            rt->input_underruns++;
        }
    }

    if (real_tape_output_active(rt) &&
        rt->output_frame_count < (int)SDL_arraysize(rt->output_frame)) {
        s16 sample = 0;
        if (ppi_port_c & 0x10) {
            int amplitude = rt->output_level * 32767 / 100;
            sample = (s16)((ppi_port_c & 0x20) ? amplitude : -amplitude);
        }
        rt->output_frame[rt->output_frame_count++] = sample;
    }
}

void real_tape_flush_output(RealTape *rt) {
    if (!rt || rt->output_frame_count <= 0) return;
    if (real_tape_output_active(rt)) {
        int bytes = rt->output_frame_count * (int)sizeof(rt->output_frame[0]);
        if (!SDL_PutAudioStreamData(rt->output_stream,
                                    rt->output_frame, bytes))
            set_error(rt, "output stream", SDL_GetError());
    }
    rt->output_frame_count = 0;
}

bool real_tape_input_active(const RealTape *rt) {
    return rt && real_tape_mode_has_input(rt->mode) && rt->input_stream;
}

bool real_tape_output_active(const RealTape *rt) {
    return rt && real_tape_mode_has_output(rt->mode) && rt->output_stream;
}

u8 real_tape_input_level(const RealTape *rt) {
    return rt ? rt->input_level : 0;
}

int real_tape_signal_percent(const RealTape *rt) {
    return rt ? tape_signal_peak_percent(&rt->input_filter) : 0;
}

int real_tape_buffered_ms(const RealTape *rt) {
    return rt ? (int)(rt->input_count * 1000 / REAL_TAPE_SAMPLE_RATE) : 0;
}

const char *real_tape_error(const RealTape *rt) {
    return rt && rt->error[0] ? rt->error : "";
}

bool real_tape_cycle_device(bool recording, const char *current, int direction,
                            char *next, size_t next_size) {
    if (!next || next_size == 0) return false;
    int count = 0;
    SDL_AudioDeviceID *devices = recording
        ? SDL_GetAudioRecordingDevices(&count)
        : SDL_GetAudioPlaybackDevices(&count);
    if (!devices && count == 0) {
        snprintf(next, next_size, "default");
        return true;
    }

    int selected = -1;
    if (!default_device(current)) {
        for (int i = 0; i < count; i++) {
            const char *name = SDL_GetAudioDeviceName(devices[i]);
            if (name && !strcmp(name, current)) {
                selected = i;
                break;
            }
        }
    }

    int next_index;
    if (direction < 0)
        next_index = selected < 0 ? count - 1 : selected - 1;
    else
        next_index = selected + 1 >= count ? -1 : selected + 1;

    if (next_index < 0) {
        snprintf(next, next_size, "default");
    } else {
        const char *name = SDL_GetAudioDeviceName(devices[next_index]);
        snprintf(next, next_size, "%s", name && name[0] ? name : "default");
    }
    SDL_free(devices);
    return true;
}

void real_tape_device_label(const char *configured,
                            char *label, size_t label_size) {
    if (!label || label_size == 0) return;
    snprintf(label, label_size, "%s",
             default_device(configured) ? "System default" : configured);
}

static bool write_wav_header(FILE *f) {
    static const u8 header[44] = {
        'R','I','F','F', 0,0,0,0, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
        0x44,0xAC,0,0,             /* 44100 Hz */
        0x88,0x58,0x01,0,          /* 88200 bytes/s */
        2,0, 16,0,
        'd','a','t','a', 0,0,0,0
    };
    return fwrite(header, 1, sizeof(header), f) == sizeof(header);
}

bool real_tape_record_start(RealTape *rt, const char *path) {
    if (!rt || !real_tape_input_active(rt) || !path || !path[0]) {
        if (rt) set_error(rt, "WAV capture", "real cassette input is not active");
        return false;
    }
    real_tape_record_stop(rt);
    rt->wav = fopen(path, "wb");
    if (!rt->wav) {
        set_error(rt, "WAV capture", "cannot open output file");
        return false;
    }
    if (!write_wav_header(rt->wav)) {
        set_error(rt, "WAV capture", "cannot write WAV header");
        fclose(rt->wav);
        rt->wav = NULL;
        return false;
    }
    snprintf(rt->wav_path, sizeof(rt->wav_path), "%s", path);
    rt->wav_bytes = 0;
    fprintf(stderr, "[real-tape] recording 44100 Hz mono s16 -> %s\n", path);
    return true;
}

bool real_tape_recording(const RealTape *rt) {
    return rt && rt->wav;
}

const char *real_tape_record_path(const RealTape *rt) {
    return rt ? rt->wav_path : "";
}

bool real_tape_ensure_wav_extension(char *path, size_t size) {
    if (!path || !path[0] || size == 0) return false;
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *base = path;
    if (slash && slash + 1 > base) base = slash + 1;
    if (backslash && backslash + 1 > base) base = backslash + 1;
    const char *dot = strrchr(base, '.');
    if (dot && !SDL_strcasecmp(dot, ".wav")) return true;
    size_t len = strlen(path);
    if (len + 4 >= size) return false;
    memcpy(path + len, ".wav", 5);
    return true;
}

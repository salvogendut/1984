#include "real_tape.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define REAL_TAPE_MONITOR_LEVEL_PERCENT 35
#define REAL_TAPE_CDT_TSTATES_PER_SAMPLE 79
#define REAL_TAPE_CDT_MAX_DATA_BYTES 0xFFFFFFu

static bool default_device(const char *name) {
    return !name || !name[0] || !strcmp(name, "default");
}

bool real_tape_mode_has_input(RealTapeMode mode) {
    return mode == REAL_TAPE_INPUT;
}

bool real_tape_mode_has_output(RealTapeMode mode) {
    return mode == REAL_TAPE_OUTPUT;
}

const char *real_tape_mode_name(RealTapeMode mode) {
    switch (mode) {
    case REAL_TAPE_INPUT:  return "input";
    case REAL_TAPE_OUTPUT: return "output";
    default:               return "off";
    }
}

bool real_tape_mode_parse(const char *text, RealTapeMode *mode) {
    if (!text || !mode) return false;
    if (!SDL_strcasecmp(text, "off"))
        *mode = REAL_TAPE_OFF;
    else if (!SDL_strcasecmp(text, "input") ||
             !SDL_strcasecmp(text, "load") ||
             !SDL_strcasecmp(text, "both"))
        *mode = REAL_TAPE_INPUT;
    else if (!SDL_strcasecmp(text, "output") ||
             !SDL_strcasecmp(text, "save"))
        *mode = REAL_TAPE_OUTPUT;
    else return false;
    return true;
}

const char *real_tape_output_source_name(RealTapeOutputSource source) {
    return source == REAL_TAPE_OUTPUT_SOURCE_CPC_SAVE
        ? "cpc_save" : "cdt";
}

bool real_tape_output_source_parse(const char *text,
                                   RealTapeOutputSource *source) {
    if (!text || !source) return false;
    if (!SDL_strcasecmp(text, "cdt"))
        *source = REAL_TAPE_OUTPUT_SOURCE_CDT;
    else if (!SDL_strcasecmp(text, "cpc_save") ||
             !SDL_strcasecmp(text, "cpc"))
        *source = REAL_TAPE_OUTPUT_SOURCE_CPC_SAVE;
    else
        return false;
    return true;
}

const char *real_tape_output_target_name(RealTapeOutputTarget target) {
    return target == REAL_TAPE_TARGET_DEVICE ? "device" : "file";
}

bool real_tape_output_target_parse(const char *text,
                                   RealTapeOutputTarget *target) {
    if (!text || !target) return false;
    if (!SDL_strcasecmp(text, "file"))
        *target = REAL_TAPE_TARGET_FILE;
    else if (!SDL_strcasecmp(text, "device"))
        *target = REAL_TAPE_TARGET_DEVICE;
    else
        return false;
    return true;
}

static void set_error(RealTape *rt, const char *side, const char *detail) {
    snprintf(rt->error, sizeof(rt->error), "%s: %s",
             side, detail && detail[0] ? detail : "unknown audio error");
    fprintf(stderr, "[real-tape] %s\n", rt->error);
}

static void clear_input_visual(RealTape *rt) {
    rt->monitor_pcm = 0;
    rt->waveform_head = 0;
    rt->waveform_count = 0;
}

static void waveform_push(RealTape *rt, s16 sample) {
    if (rt->waveform_count == REAL_TAPE_WAVEFORM_SAMPLES) {
        rt->waveform_head =
            (rt->waveform_head + 1) % REAL_TAPE_WAVEFORM_SAMPLES;
        rt->waveform_count--;
    }
    size_t tail = (rt->waveform_head + rt->waveform_count)
                % REAL_TAPE_WAVEFORM_SAMPLES;
    rt->waveform_ring[tail] = sample;
    rt->waveform_count++;
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
    rt->output_source = REAL_TAPE_OUTPUT_SOURCE_CDT;
    rt->output_target = REAL_TAPE_TARGET_FILE;
    rt->audible_monitor = true;
    rt->visual_monitor = true;
    snprintf(rt->input_device, sizeof(rt->input_device), "default");
    snprintf(rt->output_device, sizeof(rt->output_device), "default");
    tape_signal_init(&rt->input_filter);
}

static bool append_wav_samples(RealTape *rt, const s16 *samples,
                               size_t count) {
    if (!rt->capture ||
        rt->capture_format != REAL_TAPE_CAPTURE_WAV ||
        count == 0)
        return true;
    size_t bytes = count * sizeof(samples[0]);
    if (bytes > UINT32_MAX - rt->capture_bytes) {
        set_error(rt, "WAV capture", "file reached the 4 GB WAV limit");
        return false;
    }
    if (fwrite(samples, sizeof(samples[0]), count, rt->capture) != count) {
        set_error(rt, "WAV capture", "write failed");
        return false;
    }
    rt->capture_bytes += (u32)bytes;
    return true;
}

static bool start_cdt_direct_block(RealTape *rt) {
    static const u8 header[9] = {
        0x15,
        REAL_TAPE_CDT_TSTATES_PER_SAMPLE, 0,
        0, 0,       /* no pause after the converted stream */
        8,          /* patched if the final byte is partial */
        0, 0, 0,    /* patched with the packed data length */
    };
    if (fwrite(header, 1, sizeof(header), rt->capture) != sizeof(header)) {
        set_error(rt, "CDT conversion", "cannot write direct-recording block");
        return false;
    }
    rt->cdt_block_started = true;
    return true;
}

static bool write_cdt_byte(RealTape *rt, u8 byte) {
    if (rt->capture_bytes >= REAL_TAPE_CDT_MAX_DATA_BYTES) {
        set_error(rt, "CDT conversion",
                  "direct-recording block reached its 16 MB limit");
        return false;
    }
    if (fwrite(&byte, 1, 1, rt->capture) != 1) {
        set_error(rt, "CDT conversion", "write failed");
        return false;
    }
    rt->capture_bytes++;
    return true;
}

static bool append_cdt_sample(RealTape *rt, u8 tape_level) {
    if (!rt->capture || rt->capture_format != REAL_TAPE_CAPTURE_CDT)
        return true;
    if (!rt->cdt_block_started && !start_cdt_direct_block(rt))
        return false;
    if (!rt->cdt_bits &&
        rt->capture_bytes >= REAL_TAPE_CDT_MAX_DATA_BYTES) {
        set_error(rt, "CDT conversion",
                  "direct-recording block reached its 16 MB limit");
        return false;
    }

    rt->cdt_byte = (u8)((rt->cdt_byte << 1) |
                         ((tape_level & 0x80) ? 1 : 0));
    rt->cdt_bits++;
    if (rt->cdt_bits < 8) return true;

    u8 byte = rt->cdt_byte;
    rt->cdt_byte = 0;
    rt->cdt_bits = 0;
    return write_cdt_byte(rt, byte);
}

void real_tape_record_stop(RealTape *rt) {
    if (!rt || !rt->capture) return;

    RealTapeCaptureFormat format = rt->capture_format;
    if (format == REAL_TAPE_CAPTURE_WAV && rt->output_frame_count > 0) {
        int count = rt->output_frame_count;
        rt->output_frame_count = 0;
        append_wav_samples(rt, rt->output_frame, (size_t)count);
    }

    if (format == REAL_TAPE_CAPTURE_WAV) {
        if (fseek(rt->capture, 4, SEEK_SET) == 0) {
            u32 riff_size = rt->capture_bytes + 36;
            u8 b[4] = {
                (u8)riff_size, (u8)(riff_size >> 8),
                (u8)(riff_size >> 16), (u8)(riff_size >> 24)
            };
            fwrite(b, 1, sizeof(b), rt->capture);
        }
        if (fseek(rt->capture, 40, SEEK_SET) == 0) {
            u8 b[4] = {
                (u8)rt->capture_bytes, (u8)(rt->capture_bytes >> 8),
                (u8)(rt->capture_bytes >> 16),
                (u8)(rt->capture_bytes >> 24)
            };
            fwrite(b, 1, sizeof(b), rt->capture);
        }
    } else if (format == REAL_TAPE_CAPTURE_CDT &&
               rt->cdt_block_started) {
        u8 used_bits = rt->cdt_bits ? rt->cdt_bits : 8;
        if (rt->cdt_bits) {
            u8 byte = (u8)(rt->cdt_byte << (8 - rt->cdt_bits));
            write_cdt_byte(rt, byte);
        }
        if (fseek(rt->capture, 15, SEEK_SET) == 0)
            fwrite(&used_bits, 1, 1, rt->capture);
        if (fseek(rt->capture, 16, SEEK_SET) == 0) {
            u8 b[3] = {
                (u8)rt->capture_bytes,
                (u8)(rt->capture_bytes >> 8),
                (u8)(rt->capture_bytes >> 16)
            };
            fwrite(b, 1, sizeof(b), rt->capture);
        }
    }

    fclose(rt->capture);
    rt->capture = NULL;
    rt->capture_format = REAL_TAPE_CAPTURE_NONE;
    fprintf(stderr, "[real-tape] %s stopped (%u data bytes): %s\n",
            format == REAL_TAPE_CAPTURE_CDT ? "CDT conversion" : "WAV capture",
            rt->capture_bytes, rt->capture_path);
}

void real_tape_shutdown(RealTape *rt) {
    if (!rt) return;
    real_tape_record_stop(rt);
    real_tape_source_eject(rt);
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
    rt->source_sample_pos = 0;
    clear_input_visual(rt);
    tape_signal_init(&rt->input_filter);
    if (rt->input_stream) SDL_ClearAudioStream(rt->input_stream);
    if (rt->output_stream) SDL_ClearAudioStream(rt->output_stream);
}

bool real_tape_configure(RealTape *rt, RealTapeMode mode,
                         const char *input_device,
                         RealTapeOutputSource output_source,
                         RealTapeOutputTarget output_target,
                         const char *output_device,
                         int input_gain, int output_level,
                         bool audible_monitor, bool visual_monitor) {
    if (!rt) return false;
    const char *input = default_device(input_device) ? "default" : input_device;
    const char *output =
        default_device(output_device) ? "default" : output_device;
    bool mode_changed = rt->mode != mode;
    bool source_changed = rt->output_source != output_source;
    bool target_changed = rt->output_target != output_target;
    bool use_device_input = real_tape_mode_has_input(mode) &&
                            !real_tape_source_loaded(rt);
    bool reopen_input = use_device_input &&
        (!rt->input_stream || strcmp(rt->input_device, input));
    bool use_device_output =
        output_target == REAL_TAPE_TARGET_DEVICE &&
        (real_tape_mode_has_output(mode) ||
         (real_tape_mode_has_input(mode) &&
          real_tape_source_loaded(rt)));
    bool reopen_output = use_device_output &&
        (!rt->output_stream || strcmp(rt->output_device, output));
    bool visual_changed = rt->visual_monitor != visual_monitor;

    rt->error[0] = '\0';
    if (mode_changed || source_changed || target_changed)
        real_tape_record_stop(rt);
    if (mode_changed || source_changed || target_changed || reopen_output)
        real_tape_flush_output(rt);
    rt->mode = mode;
    rt->output_source = output_source;
    rt->output_target = output_target;
    rt->input_gain = input_gain < 25 ? 25 :
                     input_gain > 400 ? 400 : input_gain;
    rt->output_level = output_level < 0 ? 0 :
                       output_level > 100 ? 100 : output_level;
    rt->audible_monitor = audible_monitor;
    rt->visual_monitor = visual_monitor;
    if (visual_changed) {
        rt->waveform_head = 0;
        rt->waveform_count = 0;
    }

    if (!real_tape_mode_has_input(mode)) {
        real_tape_source_eject(rt);
        if (rt->input_stream) SDL_DestroyAudioStream(rt->input_stream);
        rt->input_stream = NULL;
        rt->active_input[0] = '\0';
        rt->input_head = 0;
        rt->input_count = 0;
        rt->input_level = 0;
        clear_input_visual(rt);
        tape_signal_init(&rt->input_filter);
    } else if (!use_device_input) {
        if (rt->input_stream) SDL_DestroyAudioStream(rt->input_stream);
        rt->input_stream = NULL;
        rt->active_input[0] = '\0';
        rt->input_head = 0;
        rt->input_count = 0;
        clear_input_visual(rt);
    } else if (reopen_input) {
        if (rt->input_stream) SDL_DestroyAudioStream(rt->input_stream);
        rt->input_stream = NULL;
        rt->input_head = 0;
        rt->input_count = 0;
        clear_input_visual(rt);
        tape_signal_init(&rt->input_filter);
        rt->input_stream = open_stream(rt, true, input,
                                       rt->active_input,
                                       sizeof(rt->active_input));
    }

    if (!use_device_output) {
        if (rt->output_stream) SDL_DestroyAudioStream(rt->output_stream);
        rt->output_stream = NULL;
        rt->active_output[0] = '\0';
        if (!real_tape_mode_has_output(mode))
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

    bool input_ok = !real_tape_mode_has_input(mode) ||
                    real_tape_source_loaded(rt) || rt->input_stream;
    bool output_ok = !use_device_output || rt->output_stream;
    return input_ok && output_ok;
}

void real_tape_pump(RealTape *rt) {
    if (!rt || !real_tape_mode_has_input(rt->mode)) return;
    if (real_tape_source_loaded(rt)) {
        if (rt->input_stream) SDL_ClearAudioStream(rt->input_stream);
        rt->input_head = 0;
        rt->input_count = 0;
        return;
    }
    if (!rt->input_stream) return;

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
        if (rt->capture &&
            rt->capture_format == REAL_TAPE_CAPTURE_WAV &&
            !append_wav_samples(rt, samples, count))
            real_tape_record_stop(rt);

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
    rt->monitor_pcm = 0;
    /* CPC cassette motor is Port C bit 4; bit 5 is its write-data line. */
    if (real_tape_mode_has_output(rt->mode) &&
        rt->output_source == REAL_TAPE_OUTPUT_SOURCE_CPC_SAVE &&
        (ppi_port_c & 0x10)) {
        real_tape_output_sample(
            rt, (ppi_port_c & 0x20) ? 0x80 : 0x00);
    }
    if (real_tape_mode_has_input(rt->mode) &&
        real_tape_source_loaded(rt)) {
        if ((ppi_port_c & 0x10) &&
            rt->source_sample_pos < rt->source_sample_count) {
            s16 sample = rt->source_samples[rt->source_sample_pos++];
            rt->input_level = tape_signal_sample(&rt->input_filter, sample,
                                                  rt->input_gain);
            rt->monitor_pcm = tape_signal_pcm(&rt->input_filter);

            if (rt->output_target == REAL_TAPE_TARGET_DEVICE &&
                rt->output_stream) {
                int scaled = (int)sample * rt->output_level / 100;
                if (rt->output_frame_count ==
                    (int)SDL_arraysize(rt->output_frame))
                    real_tape_flush_output(rt);
                if (rt->output_stream)
                    rt->output_frame[rt->output_frame_count++] =
                        (s16)scaled;
            }

            bool cdt_ok = append_cdt_sample(rt, rt->input_level);
            bool source_finished =
                rt->source_sample_pos >= rt->source_sample_count;
            if (!cdt_ok ||
                (source_finished &&
                 rt->capture_format == REAL_TAPE_CAPTURE_CDT))
                real_tape_record_stop(rt);
        } else {
            rt->input_level = 0;
        }
    } else if (real_tape_input_active(rt)) {
        if (rt->input_count > 0) {
            s16 sample = rt->input_ring[rt->input_head];
            rt->input_head =
                (rt->input_head + 1) % SDL_arraysize(rt->input_ring);
            rt->input_count--;
            rt->input_level = tape_signal_sample(&rt->input_filter, sample,
                                                  rt->input_gain);
            rt->monitor_pcm = tape_signal_pcm(&rt->input_filter);
        } else {
            rt->input_underruns++;
        }
    }
    if (rt->visual_monitor && real_tape_input_active(rt))
        waveform_push(rt, rt->monitor_pcm);
}

void real_tape_output_sample(RealTape *rt, u8 tape_level) {
    if (!rt || !real_tape_mode_has_output(rt->mode))
        return;

    int amplitude = rt->output_level * 32767 / 100;
    s16 sample = (s16)((tape_level & 0x80) ? amplitude : -amplitude);
    if (rt->visual_monitor)
        waveform_push(rt, sample);

    if (!rt->capture && !rt->output_stream)
        return;
    if (rt->output_frame_count == (int)SDL_arraysize(rt->output_frame))
        real_tape_flush_output(rt);
    if (!rt->capture && !rt->output_stream) return;

    rt->output_frame[rt->output_frame_count++] = sample;
}

void real_tape_flush_output(RealTape *rt) {
    if (!rt || rt->output_frame_count <= 0) return;
    int count = rt->output_frame_count;
    rt->output_frame_count = 0;
    if (rt->output_stream &&
        rt->output_target == REAL_TAPE_TARGET_DEVICE) {
        int bytes = count * (int)sizeof(rt->output_frame[0]);
        if (!SDL_PutAudioStreamData(rt->output_stream,
                                    rt->output_frame, bytes))
            set_error(rt, "output stream", SDL_GetError());
    }
    if (rt->capture &&
        rt->capture_format == REAL_TAPE_CAPTURE_WAV &&
        real_tape_mode_has_output(rt->mode) &&
        !append_wav_samples(rt, rt->output_frame, (size_t)count))
        real_tape_record_stop(rt);
}

bool real_tape_input_active(const RealTape *rt) {
    return rt && real_tape_mode_has_input(rt->mode) &&
           (real_tape_source_loaded(rt) || rt->input_stream);
}

bool real_tape_connected_input_active(const RealTape *rt) {
    return rt && real_tape_mode_has_input(rt->mode) &&
           !real_tape_source_loaded(rt) && rt->input_stream;
}

bool real_tape_audible_monitor_enabled(const RealTape *rt) {
    return rt && rt->audible_monitor;
}

bool real_tape_visual_monitor_enabled(const RealTape *rt) {
    return rt && rt->visual_monitor;
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

s16 real_tape_monitor_sample(const RealTape *rt) {
    if (!rt || !rt->audible_monitor ||
        !real_tape_input_active(rt))
        return 0;
    return (s16)((int)rt->monitor_pcm *
                 REAL_TAPE_MONITOR_LEVEL_PERCENT / 100);
}

size_t real_tape_waveform_copy(const RealTape *rt, s16 *samples,
                               size_t capacity) {
    if (!rt || !samples || capacity == 0) return 0;
    size_t count = rt->waveform_count;
    size_t skip = 0;
    if (count > capacity) {
        skip = count - capacity;
        count = capacity;
    }
    for (size_t i = 0; i < count; i++) {
        size_t index = (rt->waveform_head + skip + i)
                     % REAL_TAPE_WAVEFORM_SAMPLES;
        samples[i] = rt->waveform_ring[index];
    }
    return count;
}

bool real_tape_source_load_wav(RealTape *rt, const char *path) {
    if (!rt || !path || !path[0]) {
        if (rt) set_error(rt, "WAV source", "path is empty");
        return false;
    }

    SDL_AudioSpec source_spec;
    Uint8 *source_data = NULL;
    Uint32 source_len = 0;
    if (!SDL_LoadWAV(path, &source_spec, &source_data, &source_len)) {
        set_error(rt, "WAV source", SDL_GetError());
        return false;
    }
    if (source_len > INT_MAX) {
        SDL_free(source_data);
        set_error(rt, "WAV source", "audio data is too large");
        return false;
    }

    SDL_AudioSpec target_spec = {
        SDL_AUDIO_S16, 1, REAL_TAPE_SAMPLE_RATE
    };
    Uint8 *converted = NULL;
    int converted_len = 0;
    bool converted_ok = SDL_ConvertAudioSamples(
        &source_spec, source_data, (int)source_len,
        &target_spec, &converted, &converted_len);
    SDL_free(source_data);
    if (!converted_ok) {
        set_error(rt, "WAV source", SDL_GetError());
        return false;
    }
    if (converted_len < (int)sizeof(s16)) {
        SDL_free(converted);
        set_error(rt, "WAV source", "file contains no audio samples");
        return false;
    }

    real_tape_record_stop(rt);
    SDL_free(rt->source_samples);
    rt->source_samples = (s16 *)converted;
    rt->source_sample_count = (size_t)converted_len / sizeof(s16);
    rt->source_sample_pos = 0;
    rt->input_head = 0;
    rt->input_count = 0;
    rt->input_level = 0;
    clear_input_visual(rt);
    tape_signal_init(&rt->input_filter);
    if (rt->input_stream) SDL_ClearAudioStream(rt->input_stream);
    snprintf(rt->source_path, sizeof(rt->source_path), "%s", path);
    rt->error[0] = '\0';
    fprintf(stderr, "[real-tape] WAV source loaded (%zu samples): %s\n",
            rt->source_sample_count, rt->source_path);
    return true;
}

void real_tape_source_eject(RealTape *rt) {
    if (!rt) return;
    real_tape_record_stop(rt);
    real_tape_flush_output(rt);
    SDL_free(rt->source_samples);
    rt->source_samples = NULL;
    rt->source_sample_count = 0;
    rt->source_sample_pos = 0;
    rt->source_path[0] = '\0';
    rt->input_head = 0;
    rt->input_count = 0;
    rt->input_level = 0;
    clear_input_visual(rt);
    tape_signal_init(&rt->input_filter);
    if (rt->input_stream) SDL_ClearAudioStream(rt->input_stream);
}

bool real_tape_source_loaded(const RealTape *rt) {
    return rt && rt->source_samples && rt->source_sample_count > 0;
}

const char *real_tape_source_path(const RealTape *rt) {
    return rt ? rt->source_path : "";
}

int real_tape_source_progress(const RealTape *rt) {
    if (!real_tape_source_loaded(rt)) return 0;
    return (int)(rt->source_sample_pos * 100 / rt->source_sample_count);
}

u32 real_tape_source_remaining_seconds(const RealTape *rt) {
    if (!real_tape_source_loaded(rt) ||
        rt->source_sample_pos >= rt->source_sample_count)
        return 0;
    size_t remaining =
        rt->source_sample_count - rt->source_sample_pos;
    size_t seconds = remaining / REAL_TAPE_SAMPLE_RATE;
    if (remaining % REAL_TAPE_SAMPLE_RATE) seconds++;
    return seconds > UINT32_MAX ? UINT32_MAX : (u32)seconds;
}

static bool cycle_device(bool recording, const char *current, int direction,
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

bool real_tape_cycle_input_device(const char *current, int direction,
                                  char *next, size_t next_size) {
    return cycle_device(true, current, direction, next, next_size);
}

bool real_tape_cycle_output_device(const char *current, int direction,
                                   char *next, size_t next_size) {
    return cycle_device(false, current, direction, next, next_size);
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

static bool write_cdt_header(FILE *f) {
    static const u8 header[10] = {
        'Z','X','T','a','p','e','!',0x1A, 1, 20
    };
    return fwrite(header, 1, sizeof(header), f) == sizeof(header);
}

static bool path_has_extension(const char *path, const char *extension) {
    if (!path || !path[0]) return false;
    const char *slash = strrchr(path, '/');
    const char *backslash = strrchr(path, '\\');
    const char *base = path;
    if (slash && slash + 1 > base) base = slash + 1;
    if (backslash && backslash + 1 > base) base = backslash + 1;
    const char *dot = strrchr(base, '.');
    return dot && !SDL_strcasecmp(dot, extension);
}

bool real_tape_record_start(RealTape *rt, const char *path) {
    if (!rt) return false;
    bool convert_to_cdt =
        real_tape_mode_has_input(rt->mode) &&
        real_tape_source_loaded(rt);
    const char *operation =
        convert_to_cdt ? "CDT conversion" : "WAV capture";
    if (!path || !path[0]) {
        set_error(rt, operation, "output file is not selected");
        return false;
    }
    if (rt->output_target != REAL_TAPE_TARGET_FILE) {
        set_error(rt, operation, "select File as the output target");
        return false;
    }
    if (real_tape_mode_has_input(rt->mode)) {
        if (!convert_to_cdt && !real_tape_connected_input_active(rt)) {
            set_error(rt, "WAV capture", "System Audio input is not active");
            return false;
        }
    } else if (!real_tape_mode_has_output(rt->mode)) {
        set_error(rt, "WAV capture", "select INPUT or OUTPUT mode first");
        return false;
    }
    if (convert_to_cdt && !path_has_extension(path, ".cdt")) {
        set_error(rt, "CDT conversion", "output file must use .cdt");
        return false;
    }
    if (!convert_to_cdt && !path_has_extension(path, ".wav")) {
        set_error(rt, "WAV capture", "output file must use .wav");
        return false;
    }

    real_tape_record_stop(rt);
    rt->capture = fopen(path, "wb");
    if (!rt->capture) {
        set_error(rt, operation, "cannot open output file");
        return false;
    }
    bool header_ok = convert_to_cdt
        ? write_cdt_header(rt->capture)
        : write_wav_header(rt->capture);
    if (!header_ok) {
        set_error(rt, operation, "cannot write file header");
        fclose(rt->capture);
        rt->capture = NULL;
        return false;
    }
    rt->capture_format = convert_to_cdt
        ? REAL_TAPE_CAPTURE_CDT : REAL_TAPE_CAPTURE_WAV;
    snprintf(rt->capture_path, sizeof(rt->capture_path), "%s", path);
    rt->capture_bytes = 0;
    rt->cdt_byte = 0;
    rt->cdt_bits = 0;
    rt->cdt_block_started = false;
    rt->output_frame_count = 0;
    rt->error[0] = '\0';
    if (convert_to_cdt) {
        fprintf(stderr,
                "[real-tape] converting WAV to CDT direct recording -> %s\n",
                path);
    } else {
        fprintf(stderr,
                "[real-tape] recording %s 44100 Hz mono s16 -> %s\n",
                real_tape_mode_name(rt->mode), path);
    }
    return true;
}

bool real_tape_recording(const RealTape *rt) {
    return rt && rt->capture;
}

const char *real_tape_record_path(const RealTape *rt) {
    return rt ? rt->capture_path : "";
}

static bool ensure_extension(char *path, size_t size,
                             const char *extension) {
    if (!path || !path[0] || size == 0) return false;
    if (path_has_extension(path, extension)) return true;
    size_t len = strlen(path);
    size_t extension_len = strlen(extension);
    if (len + extension_len >= size) return false;
    memcpy(path + len, extension, extension_len + 1);
    return true;
}

bool real_tape_ensure_wav_extension(char *path, size_t size) {
    return ensure_extension(path, size, ".wav");
}

bool real_tape_ensure_cdt_extension(char *path, size_t size) {
    return ensure_extension(path, size, ".cdt");
}

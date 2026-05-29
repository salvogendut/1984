#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <libgen.h>
#include "config.h"
#include "overlay.h"
#include "cpc.h"
#include "mem.h"
#include "paste.h"
#include "joy.h"
#include "net4cpc.h"
#include "monitor.h"
#include "shutter_wav.h"

#define TITLE_NORMAL_464  "CPC 464  |  F4=screenshot  F5=reset  F8=monitor  F9=options  F11=fullscreen"
#define TITLE_NORMAL_6128 "CPC 6128  |  F4=screenshot  F5=reset  F8=monitor  F9=options  F11=fullscreen"
#define TITLE_CAPTURED    "Mouse captured  |  Ctrl+Enter to release"

static void set_mouse_capture(SDL_Window *win, bool capture, bool *flag, CpcModel model) {
    SDL_SetWindowRelativeMouseMode(win, capture);
    *flag = capture;
    SDL_SetWindowTitle(win, capture ? TITLE_CAPTURED
        : (model == MODEL_464 ? TITLE_NORMAL_464 : TITLE_NORMAL_6128));
}

static void usage(const char *prog, int code) {
    FILE *out = code ? stderr : stdout;
    fprintf(out,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  --464               Boot as CPC 464 (overrides config)\n"
        "  --6128              Boot as CPC 6128 (overrides config)\n"
        "  --dd1               Enable DDI-1 floppy interface on CPC 464 (overrides config)\n"
        "  --memory=KB         RAM size: 64, 128, 256, 512 or 576 (overrides config)\n"
        "  --disk-a=PATH       Mount a DSK image in drive A (overrides config)\n"
        "  --disk-b=PATH       Mount a DSK image in drive B (overrides config)\n"
        "  --rom-os=PATH       Replace the OS (lower) ROM image\n"
        "  --rom-slot=N:PATH   Load a ROM image into upper ROM slot N (0-31)\n"
        "                      May be specified multiple times\n"
        "  --trace-m4          Log every M4 board command and response to stderr\n"
        "  --trace-albireo     Log every Albireo (CH376) command and response to stderr\n"
        "                      (M4 emulation is currently unstable — see README.md)\n"
        "  --autostart=NAME    After boot, types run\"NAME into BASIC\n"
        "  --paste=TEXT        After boot, types TEXT verbatim (\\n becomes Enter)\n"
        "  --screenshot-at=N:PATH  Save a screenshot at frame N to PATH, then exit\n"
        "  --monitor-pty       Open a PTY for the memory monitor (minicom -b 9600 -D <path>)\n"
        "  -h, --help          Show this help and exit\n"
        "\n"
        "Keyboard shortcuts:\n"
        "  F4     Save screenshot (.ppm)\n"
        "  F5     Warm reset\n"
        "  F8     Open/close memory monitor / disassembler\n"
        "  F9     Options overlay\n"
        "  F11    Toggle fullscreen\n"
        "  F12    Quit\n"
        "  Ctrl+V Paste clipboard text into the emulator\n"
        "\n"
        "Configuration file: ~/.config/1984/1984.conf\n",
        prog);
    exit(code);
}

int main(int argc, char *argv[]) {

    const char *autostart       = NULL;
    const char *paste_arg       = NULL;
    const char *disk_a_arg      = NULL;
    const char *disk_b_arg      = NULL;
    const char *rom_os_arg      = NULL;
    int         screenshot_frame = -1;
    const char *screenshot_path  = NULL;
    bool        trace_io         = false;
    bool        monitor_pty      = false;
    CpcModel    model_override   = (CpcModel)-1;  /* -1 = no override */
    bool        dd1_override     = false;
    int         memory_override  = 0;             /* 0 = no override */

    /* --rom-slot=N:PATH pairs collected from CLI */
    struct { int slot; const char *path; } rom_slots[ROM_EXT_COUNT];
    int rom_slot_count = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0)
            usage(argv[0], 0);
        else if (strncmp(argv[i], "--autostart=", 12) == 0 && argv[i][12] != '\0')
            autostart = argv[i] + 12;
        else if (strncmp(argv[i], "--paste=", 8) == 0 && argv[i][8] != '\0')
            paste_arg = argv[i] + 8;
        else if (strncmp(argv[i], "--disk-a=", 9) == 0 && argv[i][9] != '\0')
            disk_a_arg = argv[i] + 9;
        else if (strncmp(argv[i], "--disk-b=", 9) == 0 && argv[i][9] != '\0')
            disk_b_arg = argv[i] + 9;
        else if (strncmp(argv[i], "--rom-os=", 9) == 0 && argv[i][9] != '\0')
            rom_os_arg = argv[i] + 9;
        else if (strncmp(argv[i], "--rom-slot=", 11) == 0 && argv[i][11] != '\0') {
            const char *arg = argv[i] + 11;
            char *colon = strchr(arg, ':');
            if (!colon || colon == arg || colon[1] == '\0') {
                fprintf(stderr, "%s: --rom-slot requires N:PATH format\n", argv[0]);
                usage(argv[0], 1);
            }
            int slot = atoi(arg);
            if (slot < 0 || slot >= ROM_EXT_COUNT) {
                fprintf(stderr, "%s: --rom-slot slot number must be 0-%d\n",
                        argv[0], ROM_EXT_COUNT - 1);
                usage(argv[0], 1);
            }
            if (rom_slot_count < ROM_EXT_COUNT) {
                rom_slots[rom_slot_count].slot = slot;
                rom_slots[rom_slot_count].path = colon + 1;
                rom_slot_count++;
            }
        } else if (strcmp(argv[i], "--464") == 0) {
            model_override = MODEL_464;
        } else if (strcmp(argv[i], "--6128") == 0) {
            model_override = MODEL_6128;
        } else if (strcmp(argv[i], "--dd1") == 0) {
            dd1_override = true;
        } else if (strncmp(argv[i], "--memory=", 9) == 0 && argv[i][9] != '\0') {
            int kb = atoi(argv[i] + 9);
            if (kb != 64 && kb != 128 && kb != 256 && kb != 512 && kb != 576) {
                fprintf(stderr, "%s: --memory=KB must be 64, 128, 256, 512 or 576\n", argv[0]);
                return 2;
            }
            memory_override = kb;
        } else if (strcmp(argv[i], "--monitor-pty") == 0) {
            monitor_pty = true;
        } else if (strcmp(argv[i], "--trace-io") == 0) {
            trace_io = true;
        } else if (strcmp(argv[i], "--trace-palette") == 0) {
            cpc_trace_palette = 1;
        } else if (strcmp(argv[i], "--trace-input") == 0) {
            cpc_trace_input = 1;
        } else if (strcmp(argv[i], "--trace-m4") == 0) {
            m4_trace = 1;
        } else if (strcmp(argv[i], "--trace-albireo") == 0) {
            ch376_trace = 1;
        } else if (strncmp(argv[i], "--screenshot-at=", 16) == 0 && argv[i][16] != '\0') {
            const char *arg = argv[i] + 16;
            char *colon = strchr(arg, ':');
            if (!colon || colon == arg || colon[1] == '\0') {
                fprintf(stderr, "%s: --screenshot-at requires N:PATH format\n", argv[0]);
                usage(argv[0], 1);
            }
            screenshot_frame = atoi(arg);
            screenshot_path  = colon + 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "%s: unrecognised option '%s'\n", argv[0], argv[i]);
            usage(argv[0], 1);
        }
    }

    Config cfg;
    if (config_load(&cfg) < 0)
        return 1;

    if (model_override != (CpcModel)-1)
        config_set_model(&cfg, model_override);
    if (memory_override)
        cfg.memory_kb = memory_override;
    if (dd1_override)
        config_apply_dd1(&cfg, true);
    if (disk_a_arg) snprintf(cfg.disk_a, sizeof(cfg.disk_a), "%s", disk_a_arg);
    if (disk_b_arg) snprintf(cfg.disk_b, sizeof(cfg.disk_b), "%s", disk_b_arg);
    if (rom_os_arg) snprintf(cfg.rom_os, sizeof(cfg.rom_os), "%s", rom_os_arg);

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    CPC cpc;
    if (cpc_init(&cpc, cfg.model, cfg.rom_os, cfg.rom_basic) < 0) {
        fprintf(stderr, "Failed to initialise CPC (check ROM paths in ~/.config/1984/1984.conf)\n");
        SDL_Quit();
        return 1;
    }
    cpc.mem.ram_size    = cfg.memory_kb * 1024;
    cpc.net4cpc         = cfg.net4cpc;
    cpc.rtc             = cfg.rtc;
    cpc.symbiface_ide   = cfg.symbiface_ide;
    cpc.symbiface_mouse = cfg.symbiface_mouse;
    cpc.m4              = cfg.m4;
    cpc.symbnet         = cfg.symbnet;
    if (cfg.m4 && cfg.m4_path[0])
        snprintf(cpc.m4_card.root, M4_PATH_MAX, "%s", cfg.m4_path);
    if (cfg.m4)
        m4_set_image(&cpc.m4_card, cfg.m4_image);
    if (cfg.symbiface_ide && cfg.ide_image[0])
        ide_open(&cpc.ide_chip, cfg.ide_image);
    cpc.albireo = cfg.albireo;
    if (cfg.albireo && cfg.albireo_image[0])
        ch376_open(&cpc.ch376, cfg.albireo_image);
    /* M4 and Albireo share the 0xFExx port range — Albireo wins if both set. */
    if (cpc.albireo && cpc.m4) {
        cpc.m4 = false;
        cfg.m4 = false;
    }

    /* Load AMSDOS ROM (non-fatal — 464 doesn't need it) */
    if (cfg.rom_amsdos[0])
        mem_load_amsdos(&cpc.mem, cfg.rom_amsdos);

    /* Load expansion ROMs into slots 0-31 (from config) */
    for (int s = 0; s < ROM_EXT_COUNT; s++) {
        /* Slot 7 is loaded below when M4 is enabled — skip any stale config entry */
        if (s == M4_ROM_SLOT && cfg.m4) continue;
        if (cfg.rom_ext[s][0])
            mem_load_rom_ext(&cpc.mem, s, cfg.rom_ext[s]);
    }
    /* Load M4ROM into its dedicated slot when M4 is enabled */
    if (cfg.m4) {
        char m4rom[512];
        config_default_m4rom(m4rom, sizeof(m4rom));
        mem_load_rom_ext(&cpc.mem, M4_ROM_SLOT, m4rom);
        /* Seed the M4 board's config buffer (read via bus bypass at 0xF400)
         * with the ROM's default contents so reads return valid pointers
         * (e.g. runfile_ptr → autoexec_fn) before any C_CONFIG write. */
        memcpy(cpc.m4_card.cfg_mem,
               &cpc.mem.rom_ext[M4_ROM_SLOT][0xF400 - 0xC000],
               sizeof(cpc.m4_card.cfg_mem));
    }

    /* Apply --rom-slot=N:PATH overrides from CLI */
    for (int i = 0; i < rom_slot_count; i++) {
        if (mem_load_rom_ext(&cpc.mem, rom_slots[i].slot, rom_slots[i].path) < 0)
            fprintf(stderr, "1984: failed to load ROM slot %d: %s\n",
                    rom_slots[i].slot, rom_slots[i].path);
    }

    /* Load floppy images from config */
    if (cfg.disk_a[0]) {
        if (disk_load(&cpc.drive[0], cfg.disk_a) < 0) {
            fprintf(stderr, "1984: failed to load drive A: %s\n", cfg.disk_a);
            cfg.disk_a[0] = '\0';
        }
    }
    if (cfg.disk_b[0]) {
        if (disk_load(&cpc.drive[1], cfg.disk_b) < 0) {
            fprintf(stderr, "1984: failed to load drive B: %s\n", cfg.disk_b);
            cfg.disk_b[0] = '\0';
        }
    }

    /* Camera shutter SFX — loaded from embedded WAV data in shutter_wav.h.
     * sfx_buf/sfx_buf_len hold the decoded PCM so it can be replayed on each F4 press.
     * A dedicated logical device stream is opened; SDL3 mixes it with the PSG stream. */
    SDL_AudioStream *sfx_stream = NULL;
    Uint8  *sfx_buf     = NULL;
    Uint32  sfx_buf_len = 0;
    {
        SDL_AudioSpec sfx_spec;
        SDL_IOStream *io = SDL_IOFromConstMem(shutter_wav, shutter_wav_len);
        if (io && SDL_LoadWAV_IO(io, true, &sfx_spec, &sfx_buf, &sfx_buf_len)) {
            sfx_stream = SDL_OpenAudioDeviceStream(
                SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &sfx_spec, NULL, NULL);
            if (sfx_stream)
                SDL_ResumeAudioStreamDevice(sfx_stream);
        }
    }

    Overlay overlay;
    overlay_init(&overlay, &cfg, &cpc);

    Monitor *monitor = monitor_create(&cpc);
    if (monitor_pty) {
        const char *pty_path = monitor_pty_open(monitor);
        if (pty_path)
            fprintf(stderr, "1984: monitor PTY: %s  (minicom -b 9600 -D %s)\n",
                    pty_path, pty_path);
        else
            fprintf(stderr, "1984: failed to open monitor PTY\n");
    }

    Paste paste;
    paste_init(&paste);

    Joy joy;
    joy_init(&joy);

    bool fullscreen     = cfg.fullscreen;
    bool mouse_captured = false;
    if (fullscreen)
        SDL_SetWindowFullscreen(cpc.display.window, true);

    /* Frames to wait before injecting autostart text; 200 ≈ 4 s at 50 Hz */
    int autostart_countdown = (autostart || paste_arg) ? 200 : 0;

    /* 50 Hz frame pacer — audio is pushed every 20 ms, matching the CPC's PAL rate.
     * VSync is off; we sleep for any leftover time in each 20 ms budget. */
#define FRAME_NS 20000000ULL
    Uint64 next_frame = SDL_GetTicksNS();

    int  frame_count = 0;
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
                continue;
            }

            /* Mouse events when captured — route to CPC mouse state, skip other handlers */
            if (mouse_captured) {
                if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                    if (cpc.symbiface_mouse)
                        mouse_move(&cpc.mouse, (int)ev.motion.xrel, (int)ev.motion.yrel);
                    continue;
                }
                if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                    ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                    if (cpc.symbiface_mouse) {
                        int btn = (ev.button.button == SDL_BUTTON_LEFT)   ? 0 :
                                  (ev.button.button == SDL_BUTTON_RIGHT)  ? 1 :
                                  (ev.button.button == SDL_BUTTON_MIDDLE) ? 2 : -1;
                        if (btn >= 0)
                            mouse_button(&cpc.mouse, btn,
                                         ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
                    }
                    continue;
                }
                if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                    if (cpc.symbiface_mouse)
                        mouse_scroll(&cpc.mouse, (int)ev.wheel.y);
                    continue;
                }
                /* Ctrl+Enter releases mouse capture — do not pass Enter to CPC */
                if (ev.type == SDL_EVENT_KEY_DOWN &&
                    ev.key.scancode == SDL_SCANCODE_RETURN &&
                    (SDL_GetModState() & SDL_KMOD_CTRL)) {
                    set_mouse_capture(cpc.display.window, false,
                                      &mouse_captured, cpc.model);
                    continue;
                }
            }

            /* Click in emulator window captures mouse when mouse support is enabled */
            if (!mouse_captured && cpc.symbiface_mouse && !overlay.visible &&
                ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                ev.button.windowID == SDL_GetWindowID(cpc.display.window)) {
                set_mouse_capture(cpc.display.window, true,
                                  &mouse_captured, cpc.model);
                /* Also feed this button press into the mouse state */
                int btn = (ev.button.button == SDL_BUTTON_LEFT)   ? 0 :
                          (ev.button.button == SDL_BUTTON_RIGHT)  ? 1 :
                          (ev.button.button == SDL_BUTTON_MIDDLE) ? 2 : -1;
                if (btn >= 0)
                    mouse_button(&cpc.mouse, btn, true);
                continue;
            }

            /* Joystick/gamepad events */
            if (joy_handle_event(&joy, &ev, &cpc.kbd))
                continue;
            /* Monitor window gets its own events */
            if (monitor_handle_event(monitor, &ev))
                continue;
            /* F9 opens overlay — release mouse capture first so UI is usable */
            if (mouse_captured && ev.type == SDL_EVENT_KEY_DOWN &&
                    ev.key.scancode == SDL_SCANCODE_F9)
                set_mouse_capture(cpc.display.window, false,
                                  &mouse_captured, cpc.model);
            /* Overlay gets first crack at every key event */
            if (overlay_handle_event(&overlay, &ev))
                continue;
            /* Pass remaining key events to the CPC */
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                if (ev.key.scancode == SDL_SCANCODE_F12) {
                    running = false;
                } else if (ev.key.scancode == SDL_SCANCODE_F8) {
                    if (monitor_is_open(monitor))
                        monitor_handle_event(monitor,
                            &(SDL_Event){.type=SDL_EVENT_WINDOW_CLOSE_REQUESTED,
                                         .window.windowID=monitor_window_id(monitor)});
                    else
                        monitor_open(monitor);
                } else if (ev.key.scancode == SDL_SCANCODE_F11) {
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(cpc.display.window, fullscreen);
                } else if (ev.key.scancode == SDL_SCANCODE_F4) {
                    char path[256];
                    char tmp[256];
                    strncpy(tmp, argv[0], sizeof(tmp) - 1);
                    tmp[sizeof(tmp) - 1] = '\0';
                    snprintf(path, sizeof(path), "%s_%ld.ppm", basename(tmp), (long)time(NULL));
                    display_save_ppm(&cpc.display, path);
                    if (sfx_stream && sfx_buf) {
                        SDL_ClearAudioStream(sfx_stream);
                        SDL_PutAudioStreamData(sfx_stream, sfx_buf, (int)sfx_buf_len);
                    }
                } else if (ev.key.scancode == SDL_SCANCODE_F5) {
                    cpc_reset(&cpc);
                } else if (ev.key.scancode == SDL_SCANCODE_V &&
                           (SDL_GetModState() & SDL_KMOD_CTRL)) {
                    /* Release Ctrl from the CPC matrix before injecting text;
                     * otherwise the first character arrives as Ctrl+key. */
                    cpc_key_event(&cpc, SDL_SCANCODE_LCTRL, false);
                    cpc_key_event(&cpc, SDL_SCANCODE_RCTRL, false);
                    char *text = SDL_GetClipboardText();
                    if (text) { paste_text(&paste, text); SDL_free(text); }
                } else {
                    if (cpc_trace_input)
                        fprintf(stderr, "[input] KEY_DOWN scancode=%d name=%s\n",
                                ev.key.scancode, SDL_GetScancodeName(ev.key.scancode));
                    cpc_key_event(&cpc, ev.key.scancode, true);
                }
            } else if (ev.type == SDL_EVENT_KEY_UP) {
                if (cpc_trace_input)
                    fprintf(stderr, "[input] KEY_UP   scancode=%d\n", ev.key.scancode);
                cpc_key_event(&cpc, ev.key.scancode, false);
            }
        }

        if (autostart_countdown > 0 && --autostart_countdown == 0) {
            if (paste_arg) {
                /* --paste=TEXT: expand \n to real newline then inject */
                char buf[512];
                int j = 0;
                for (const char *p = paste_arg; *p && j < (int)sizeof(buf)-2; p++) {
                    if (p[0] == '\\' && p[1] == 'n') { buf[j++] = '\r'; buf[j++] = '\n'; p++; }
                    else buf[j++] = *p;
                }
                buf[j] = '\0';
                paste_text(&paste, buf);
            } else {
                char cmd[256];
                snprintf(cmd, sizeof(cmd), "run\"%s", autostart);
                paste_text(&paste, cmd);
            }
        }

        overlay_tick(&overlay);

        if (overlay.needs_cold_boot) {
            overlay.needs_cold_boot = false;
            cpc.model = cfg.model;
            cpc.mem.ram_size = cfg.memory_kb * 1024;
            mem_load_rom(&cpc.mem, cfg.rom_os, cfg.rom_basic);
            if (cfg.rom_amsdos[0] && cfg.dd1)
                mem_load_amsdos(&cpc.mem, cfg.rom_amsdos);
            else if (!cfg.dd1 && cpc.model == MODEL_464)
                mem_unload_amsdos(&cpc.mem);
            for (int s = 0; s < ROM_EXT_COUNT; s++) {
                if (s == M4_ROM_SLOT && cfg.m4) continue;
                if (cfg.rom_ext[s][0])
                    mem_load_rom_ext(&cpc.mem, s, cfg.rom_ext[s]);
            }
            if (cfg.m4) {
                char m4rom[512];
                config_default_m4rom(m4rom, sizeof(m4rom));
                mem_load_rom_ext(&cpc.mem, M4_ROM_SLOT, m4rom);
                memcpy(cpc.m4_card.cfg_mem,
                       &cpc.mem.rom_ext[M4_ROM_SLOT][0xF400 - 0xC000],
                       sizeof(cpc.m4_card.cfg_mem));
            }
            const char *title = (cpc.model == MODEL_464)
                ? TITLE_NORMAL_464 : TITLE_NORMAL_6128;
            SDL_SetWindowTitle(cpc.display.window, title);
            cpc.net4cpc          = cfg.net4cpc;
            cpc.rtc              = cfg.rtc;
            cpc.symbiface_ide    = cfg.symbiface_ide;
            cpc.symbiface_mouse  = cfg.symbiface_mouse;
            cpc.m4               = cfg.m4;
            cpc.symbnet          = cfg.symbnet;
            if (cfg.m4 && cfg.m4_path[0])
                snprintf(cpc.m4_card.root, M4_PATH_MAX, "%s", cfg.m4_path);
            if (cfg.m4)
                m4_set_image(&cpc.m4_card, cfg.m4_image);
            ide_close(&cpc.ide_chip);
            if (cfg.symbiface_ide && cfg.ide_image[0])
                ide_open(&cpc.ide_chip, cfg.ide_image);
            cpc.albireo = cfg.albireo;
            ch376_close(&cpc.ch376);
            if (cfg.albireo && cfg.albireo_image[0])
                ch376_open(&cpc.ch376, cfg.albireo_image);
            if (cpc.albireo && cpc.m4) {
                cpc.m4 = false;
                cfg.m4 = false;
            }
            /* Release mouse capture on cold boot */
            if (mouse_captured)
                set_mouse_capture(cpc.display.window, false,
                                  &mouse_captured, cpc.model);
            net4cpc_reset();
            cpc_reset(&cpc);
        }

        monitor_pty_tick(monitor);
        paste_tick(&paste, &cpc.kbd);
        bool was_paused   = cpc.paused;
        bool was_stepping = cpc.step_once;
        cpc_frame(&cpc);
        /* Auto-open monitor on breakpoint hit */
        if (!was_paused && cpc.paused) {
            monitor_open(monitor);
            monitor_notify_break(monitor);
        } else if (was_stepping && cpc.paused) {
            monitor_notify_step(monitor);
        }
        overlay_render(&overlay, cpc.display.renderer);
        display_flip(&cpc.display);
        monitor_render(monitor);

        frame_count++;
        if (trace_io && frame_count == 210)
            cpc_trace_io = 1;
        if (trace_io && frame_count == 600)
            cpc_trace_io = 0;
        /* At frame 700, dump what the GA palette looks like and sample screen */
        if (trace_io && frame_count == 700) {
            fprintf(stderr, "=== GA palette (inks) ===\n");
            for (int i = 0; i <= 16; i++)
                fprintf(stderr, "  ink[%2d] = hw%d\n", i, cpc.ga.ink[i]);
            /* Histogram of screen byte values */
            int hist[256] = {0};
            for (int i = 0; i < 0x4000; i++) hist[cpc.mem.ram[0xC000+i]]++;
            fprintf(stderr, "=== Screen byte histogram (non-zero) ===\n");
            for (int i = 1; i < 256; i++)
                if (hist[i]) fprintf(stderr, "  [%02X] = %d\n", i, hist[i]);
        }
        if (screenshot_frame >= 0 && frame_count == screenshot_frame) {
            display_save_ppm(&cpc.display, screenshot_path);
            running = false;
        }

        /* Sleep for whatever is left of the 20 ms frame budget */
        next_frame += FRAME_NS;
        Uint64 now = SDL_GetTicksNS();
        if (now < next_frame) {
            SDL_DelayNS(next_frame - now);
        } else if (now > next_frame + 3 * FRAME_NS) {
            next_frame = now; /* reset if more than 3 frames behind */
        }
    }

    if (mouse_captured)
        SDL_SetWindowRelativeMouseMode(cpc.display.window, false);
    paste_free(&paste);
    joy_destroy(&joy);
    monitor_destroy(monitor);
    if (sfx_stream) SDL_DestroyAudioStream(sfx_stream);
    if (sfx_buf)    SDL_free(sfx_buf);
    cpc_destroy(&cpc);
    SDL_Quit();
    return 0;
}

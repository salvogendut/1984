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

static void usage(const char *prog, int code) {
    FILE *out = code ? stderr : stdout;
    fprintf(out,
        "Usage: %s [OPTIONS]\n"
        "\n"
        "Options:\n"
        "  --disk-a=PATH       Mount a DSK image in drive A (overrides config)\n"
        "  --disk-b=PATH       Mount a DSK image in drive B (overrides config)\n"
        "  --autostart=NAME    After boot, types run\"NAME into BASIC\n"
        "  --paste=TEXT        After boot, types TEXT verbatim (\\n becomes Enter)\n"
        "  -h, --help          Show this help and exit\n"
        "\n"
        "Keyboard shortcuts:\n"
        "  F4     Save screenshot\n"
        "  F5     Warm reset\n"
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

    const char *autostart  = NULL;
    const char *paste_arg  = NULL;
    const char *disk_a_arg = NULL;
    const char *disk_b_arg = NULL;
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
        else if (argv[i][0] == '-') {
            fprintf(stderr, "%s: unrecognised option '%s'\n", argv[0], argv[i]);
            usage(argv[0], 1);
        }
    }

    Config cfg;
    if (config_load(&cfg) < 0)
        return 1;

    if (disk_a_arg) snprintf(cfg.disk_a, sizeof(cfg.disk_a), "%s", disk_a_arg);
    if (disk_b_arg) snprintf(cfg.disk_b, sizeof(cfg.disk_b), "%s", disk_b_arg);

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

    /* Load AMSDOS ROM (non-fatal — 464 doesn't need it) */
    if (cfg.rom_amsdos[0])
        mem_load_amsdos(&cpc.mem, cfg.rom_amsdos);

    /* Load expansion ROMs into slots 0-31 */
    for (int s = 0; s < ROM_EXT_COUNT; s++) {
        if (cfg.rom_ext[s][0])
            mem_load_rom_ext(&cpc.mem, s, cfg.rom_ext[s]);
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

    Overlay overlay;
    overlay_init(&overlay, &cfg, &cpc);

    Paste paste;
    paste_init(&paste);

    Joy joy;
    joy_init(&joy);

    bool fullscreen = cfg.fullscreen;
    if (fullscreen)
        SDL_SetWindowFullscreen(cpc.display.window, true);

    /* Frames to wait before injecting autostart text; 200 ≈ 4 s at 50 Hz */
    int autostart_countdown = (autostart || paste_arg) ? 200 : 0;

    /* 50 Hz frame pacer — audio is pushed every 20 ms, matching the CPC's PAL rate.
     * VSync is off; we sleep for any leftover time in each 20 ms budget. */
#define FRAME_NS 20000000ULL
    Uint64 next_frame = SDL_GetTicksNS();

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
                continue;
            }
            /* Joystick/gamepad events */
            if (joy_handle_event(&joy, &ev, &cpc.kbd))
                continue;
            /* Overlay gets first crack at every key event */
            if (overlay_handle_event(&overlay, &ev))
                continue;
            /* Pass remaining key events to the CPC */
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                if (ev.key.scancode == SDL_SCANCODE_F12) {
                    running = false;
                } else if (ev.key.scancode == SDL_SCANCODE_F11) {
                    fullscreen = !fullscreen;
                    SDL_SetWindowFullscreen(cpc.display.window, fullscreen);
                } else if (ev.key.scancode == SDL_SCANCODE_F4) {
                    char path[256];
                    char tmp[256];
                    strncpy(tmp, argv[0], sizeof(tmp) - 1);
                    tmp[sizeof(tmp) - 1] = '\0';
                    snprintf(path, sizeof(path), "%s_%ld.png", basename(tmp), (long)time(NULL));
                    display_save_png(&cpc.display, path);
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
                    cpc_key_event(&cpc, ev.key.scancode, true);
                }
            } else if (ev.type == SDL_EVENT_KEY_UP) {
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
            mem_load_rom(&cpc.mem, cfg.rom_os, cfg.rom_basic);
            if (cfg.rom_amsdos[0] && cfg.dd1)
                mem_load_amsdos(&cpc.mem, cfg.rom_amsdos);
            else if (!cfg.dd1 && cpc.model == MODEL_464)
                mem_unload_amsdos(&cpc.mem);
            for (int s = 0; s < ROM_EXT_COUNT; s++) {
                if (cfg.rom_ext[s][0])
                    mem_load_rom_ext(&cpc.mem, s, cfg.rom_ext[s]);
            }
            const char *title = (cpc.model == MODEL_464)
                ? "CPC 464  |  F4 = screenshot   F5 = reset   F9 = options   F11 = fullscreen"
                : "CPC 6128  |  F4 = screenshot   F5 = reset   F9 = options   F11 = fullscreen";
            SDL_SetWindowTitle(cpc.display.window, title);
            cpc_reset(&cpc);
        }

        paste_tick(&paste, &cpc.kbd);
        cpc_frame(&cpc);
        overlay_render(&overlay, cpc.display.renderer);
        display_flip(&cpc.display);

        /* Sleep for whatever is left of the 20 ms frame budget */
        next_frame += FRAME_NS;
        Uint64 now = SDL_GetTicksNS();
        if (now < next_frame) {
            SDL_DelayNS(next_frame - now);
        } else if (now > next_frame + 3 * FRAME_NS) {
            next_frame = now; /* reset if more than 3 frames behind */
        }
    }

    paste_free(&paste);
    joy_destroy(&joy);
    cpc_destroy(&cpc);
    SDL_Quit();
    return 0;
}

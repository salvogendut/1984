#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdbool.h>
#include "config.h"
#include "overlay.h"
#include "cpc.h"
#include "paste.h"

int main(int argc, char *argv[]) {
    Config cfg;
    if (config_load(&cfg) < 0)
        return 1;

    (void)argc; (void)argv;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    CPC cpc;
    if (cpc_init(&cpc, cfg.model, cfg.rom_os, cfg.rom_basic) < 0) {
        fprintf(stderr, "Failed to initialise CPC (check ROM paths in ~/.config/1984/1984.conf)\n");
        SDL_Quit();
        return 1;
    }

    Overlay overlay;
    overlay_init(&overlay, &cfg);

    Paste paste;
    paste_init(&paste);

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                running = false;
                continue;
            }
            /* Overlay gets first crack at every key event */
            if (overlay_handle_event(&overlay, &ev))
                continue;
            /* Pass remaining key events to the CPC */
            if (ev.type == SDL_EVENT_KEY_DOWN) {
                if (ev.key.scancode == SDL_SCANCODE_F12) {
                    running = false;
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

        paste_tick(&paste, &cpc.kbd);
        cpc_frame(&cpc);
        overlay_render(&overlay, cpc.display.renderer);
        display_flip(&cpc.display);
    }

    paste_free(&paste);
    cpc_destroy(&cpc);
    SDL_Quit();
    return 0;
}

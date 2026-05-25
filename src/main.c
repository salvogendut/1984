#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdbool.h>
#include "config.h"
#include "cpc.h"

int main(int argc, char *argv[]) {
    Config cfg;
    if (config_load(&cfg) < 0)
        return 1;

    /* CLI arg '4' overrides model for quick testing */
    if (argc > 1 && argv[1][0] == '4')
        cfg.model = MODEL_464;

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

    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_EVENT_QUIT:
                    running = false;
                    break;
                case SDL_EVENT_KEY_DOWN:
                    if (ev.key.scancode == SDL_SCANCODE_F12)
                        running = false;
                    else
                        cpc_key_event(&cpc, ev.key.scancode, true);
                    break;
                case SDL_EVENT_KEY_UP:
                    cpc_key_event(&cpc, ev.key.scancode, false);
                    break;
            }
        }

        cpc_frame(&cpc);
    }

    cpc_destroy(&cpc);
    SDL_Quit();
    return 0;
}

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdbool.h>
#include "cpc.h"

#define ROM_OS_464     "roms/OS_464.ROM"
#define ROM_BASIC_464  "roms/BASIC_1.0.ROM"
#define ROM_OS_6128    "roms/OS_6128.ROM"
#define ROM_BASIC_6128 "roms/BASIC_1.1.ROM"

int main(int argc, char *argv[]) {
    CpcModel model = MODEL_6128;
    if (argc > 1 && argv[1][0] == '4')
        model = MODEL_464;

    const char *rom_os    = (model == MODEL_464) ? ROM_OS_464    : ROM_OS_6128;
    const char *rom_basic = (model == MODEL_464) ? ROM_BASIC_464 : ROM_BASIC_6128;

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    CPC cpc;
    if (cpc_init(&cpc, model, rom_os, rom_basic) < 0) {
        fprintf(stderr, "Failed to initialise CPC (check ROM files in roms/)\n");
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

#include "display.h"
#include <string.h>
#include <stdio.h>

int display_init(Display *d, const char *title) {
    memset(d, 0, sizeof(*d));

    d->window = SDL_CreateWindow(
        title,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        CPC_SCREEN_W * WINDOW_SCALE, CPC_SCREEN_H * WINDOW_SCALE,
        SDL_WINDOW_RESIZABLE
    );
    if (!d->window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }

    d->renderer = SDL_CreateRenderer(d->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!d->renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }

    SDL_RenderSetLogicalSize(d->renderer, CPC_SCREEN_W, CPC_SCREEN_H);

    d->texture = SDL_CreateTexture(d->renderer,
        SDL_PIXELFORMAT_XRGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        CPC_SCREEN_W, CPC_SCREEN_H);
    if (!d->texture) {
        fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        return -1;
    }

    memset(d->pixels, 0, sizeof(d->pixels));
    return 0;
}

void display_destroy(Display *d) {
    if (d->texture)  SDL_DestroyTexture(d->texture);
    if (d->renderer) SDL_DestroyRenderer(d->renderer);
    if (d->window)   SDL_DestroyWindow(d->window);
}

void display_put_pixel(Display *d, u32 rgb) {
    if (d->scan_x < CPC_SCREEN_W && d->scan_y < CPC_SCREEN_H)
        d->pixels[d->scan_y * CPC_SCREEN_W + d->scan_x] = rgb;
    d->scan_x++;
}

void display_next_line(Display *d) {
    d->scan_x = 0;
    d->scan_y++;
}

void display_vsync(Display *d) {
    d->scan_x = 0;
    d->scan_y = 0;
}

void display_present(Display *d) {
    SDL_UpdateTexture(d->texture, NULL, d->pixels, CPC_SCREEN_W * sizeof(u32));
    SDL_RenderClear(d->renderer);
    SDL_RenderCopy(d->renderer, d->texture, NULL, NULL);
    SDL_RenderPresent(d->renderer);
}

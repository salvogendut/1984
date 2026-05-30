#include "display.h"
#include <string.h>
#include <stdio.h>

int display_init(Display *d, const char *title) {
    memset(d, 0, sizeof(*d));

    d->window = SDL_CreateWindow(title, WINDOW_W, WINDOW_H, SDL_WINDOW_RESIZABLE);
    if (!d->window) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }

    d->renderer = SDL_CreateRenderer(d->window, NULL);
    if (!d->renderer) {
        fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetRenderVSync(d->renderer, 0); /* timing is done by the 50 Hz software pacer */

    /* No logical size — we'll blit with explicit dest rect for 4:3 */

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

void display_upload(Display *d) {
    int ww, wh;
    SDL_GetWindowSize(d->window, &ww, &wh);
    /* Fit 768×272 into the window maintaining 4:3 (WINDOW_W:WINDOW_H) ratio */
    int dst_w = ww;
    int dst_h = wh;
    if (dst_w * WINDOW_H > dst_h * WINDOW_W)
        dst_w = dst_h * WINDOW_W / WINDOW_H;
    else
        dst_h = dst_w * WINDOW_H / WINDOW_W;
    SDL_FRect dst = { (float)(ww - dst_w) / 2, (float)(wh - dst_h) / 2, (float)dst_w, (float)dst_h };

    SDL_UpdateTexture(d->texture, NULL, d->pixels, CPC_SCREEN_W * sizeof(u32));
    /* Force the clear colour to opaque black before clearing — the overlay
     * leaves whatever it last drew with (often a yellow text colour or a
     * tab-highlight blue) as the renderer's draw colour, and an unset
     * clear leaks that into the letterbox gutter in fullscreen. */
    SDL_SetRenderDrawColor(d->renderer, 0, 0, 0, 255);
    SDL_RenderClear(d->renderer);
    SDL_RenderTexture(d->renderer, d->texture, NULL, &dst);
}

void display_flip(Display *d) {
    SDL_RenderPresent(d->renderer);
}

void display_save_ppm(Display *d, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;

    fprintf(f, "P6\n%d %d\n255\n", CPC_SCREEN_W, WINDOW_H);

    /* Scale vertically from CPC_SCREEN_H to WINDOW_H for correct 4:3 aspect */
    for (int y = 0; y < WINDOW_H; y++) {
        int src_y = y * CPC_SCREEN_H / WINDOW_H;
        for (int x = 0; x < CPC_SCREEN_W; x++) {
            u32 px = d->pixels[src_y * CPC_SCREEN_W + x];
            unsigned char rgb[3] = {
                (px >> 16) & 0xFF,
                (px >>  8) & 0xFF,
                 px        & 0xFF,
            };
            fwrite(rgb, 1, 3, f);
        }
    }

    fclose(f);
}

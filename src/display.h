#pragma once
#include "types.h"
#include <SDL3/SDL.h>
#include <stdbool.h>

/*
 * SDL3 display layer.
 * The CPC screen is rendered into a 768x272 pixel texture (mode 2 native
 * resolution doubled horizontally for modes 0/1 visibility) and scaled
 * to the window.
 */

#define CPC_SCREEN_W    768
#define CPC_SCREEN_H    272
#define WINDOW_W        768   /* 4:3 display width */
#define WINDOW_H        576   /* 4:3 display height (768 × 3/4) */

typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;
    u32           pixels[CPC_SCREEN_W * CPC_SCREEN_H];
    int           scan_x;
    int           scan_y;
} Display;

int  display_init(Display *d, const char *title);
void display_destroy(Display *d);
void display_put_pixel(Display *d, u32 rgb);
void display_next_line(Display *d);
void display_vsync(Display *d);
void display_upload(Display *d);   /* update texture + blit to renderer (no flip) */
void display_flip(Display *d);     /* SDL_RenderPresent */
void display_save_ppm(Display *d, const char *path);

/* Switch texture scaling between linear (smooth) and nearest (sharp). */
void display_set_smoothing(Display *d, bool smooth);

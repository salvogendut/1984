#pragma once
#include "types.h"
#include <SDL2/SDL.h>

/*
 * SDL2 display layer.
 * The CPC screen is rendered into a 768x272 pixel texture (mode 2 native
 * resolution doubled horizontally for modes 0/1 visibility) and scaled
 * to the window.
 */

#define CPC_SCREEN_W  768
#define CPC_SCREEN_H  272
#define WINDOW_SCALE    2

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
void display_put_pixel(Display *d, u32 rgb);  /* called per Gate Array pixel */
void display_next_line(Display *d);
void display_vsync(Display *d);               /* flip and reset raster position */
void display_present(Display *d);

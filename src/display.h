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
#define LED_BAR_HEIGHT  22    /* drive-activity LED strip below the CPC area */
#define WINDOW_H_TOTAL  (WINDOW_H + LED_BAR_HEIGHT)
#define DISPLAY_CRT_SCANLINES_DEFAULT 35
#define DISPLAY_CRT_BRIGHTNESS_DEFAULT 100
#define DISPLAY_CRT_CONTRAST_DEFAULT 100
#define DISPLAY_CRT_RGB_DEFAULT 100

typedef struct {
    SDL_Window   *window;
    SDL_Renderer *renderer;
    SDL_Texture  *texture;
    u32           pixels[CPC_SCREEN_W * CPC_SCREEN_H];
    u32           crt_pixels[CPC_SCREEN_W * CPC_SCREEN_H];
    int           scan_x;
    int           scan_y;
    bool          crt_enabled;
    int           crt_scanlines;
    int           crt_brightness;
    int           crt_contrast;
    int           crt_red;
    int           crt_green;
    int           crt_blue;
} Display;

int  display_init(Display *d, const char *title, int scale);
void display_destroy(Display *d);
void display_put_pixel(Display *d, u32 rgb);
void display_next_line(Display *d);
void display_vsync(Display *d);
void display_upload(Display *d);   /* update texture + blit to renderer (no flip) */
void display_flip(Display *d);     /* SDL_RenderPresent */
void display_save_ppm(Display *d, const char *path);
u32  display_hash(Display *d);
void display_copy_visible(Display *d, u32 *dst);
bool display_changed_rect(Display *d, u32 *prev, int *x, int *y, int *w, int *h);
bool display_save_crop_ppm(Display *d, const char *path,
                           int x, int y, int w, int h, int scale);

/* Switch texture scaling between linear (smooth) and nearest (sharp). */
void display_set_smoothing(Display *d, bool smooth);
void display_set_crt(Display *d, bool enabled, int scanlines, int brightness,
                     int contrast, int red, int green, int blue);

/* In-place desaturate the current framebuffer (Rec. 601 luma). Used when
 * the emulator pauses so the frozen frame visibly differs from a running
 * one. The next live frame from cpc_frame() overwrites this. */
void display_apply_greyscale(Display *d);

/* Draw a "PAUSED" label centred on the renderer. Call after
 * display_upload(), before display_flip(). */
void display_draw_paused_label(Display *d);

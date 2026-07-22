/* ffmpeg_gif.c - optional global-palette GIF optimization through FFmpeg. */
#include "ffmpeg_gif.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool ffmpeg_gif_optimize(const char *path) {
#ifndef HAVE_FFMPEG
    (void)path;
    fprintf(stderr, "[ffmpeg-gif] not compiled with FFmpeg support\n");
    return false;
#else
    if (!path || !path[0])
        return false;

    size_t temp_len = strlen(path) + sizeof(".ffmpeg.tmp.gif");
    char *temp = malloc(temp_len);
    if (!temp)
        return false;
    snprintf(temp, temp_len, "%s.ffmpeg.tmp.gif", path);
    SDL_RemovePath(temp);  /* ignore a missing stale temporary file */

    const char *args[] = {
        FFMPEG_PATH,
        "-y", "-loglevel", "error",
        "-i", path,
        "-filter_complex",
        "[0:v]split[palette_src][pixel_src];"
        "[palette_src]palettegen=max_colors=256:reserve_transparent=0[palette];"
        "[pixel_src][palette]paletteuse=dither=none",
        "-fps_mode", "passthrough",
        "-gifflags", "+offsetting+transdiff",
        "-loop", "0",
        temp,
        NULL
    };

    fprintf(stderr, "[ffmpeg-gif] optimizing %s\n", path);
    SDL_Process *process = SDL_CreateProcess(args, false);
    if (!process) {
        fprintf(stderr, "[ffmpeg-gif] could not start '%s': %s\n",
                FFMPEG_PATH, SDL_GetError());
        free(temp);
        return false;
    }

    int exit_code = -255;
    bool exited = SDL_WaitProcess(process, true, &exit_code);
    SDL_DestroyProcess(process);
    if (!exited || exit_code != 0) {
        fprintf(stderr, "[ffmpeg-gif] conversion failed (exit=%d)\n",
                exit_code);
        SDL_RemovePath(temp);
        free(temp);
        return false;
    }

    SDL_PathInfo original_info;
    SDL_PathInfo optimized_info;
    if (!SDL_GetPathInfo(path, &original_info) ||
        !SDL_GetPathInfo(temp, &optimized_info)) {
        fprintf(stderr, "[ffmpeg-gif] could not compare output sizes: %s\n",
                SDL_GetError());
        SDL_RemovePath(temp);
        free(temp);
        return false;
    }

    if (optimized_info.size == 0 ||
        optimized_info.size >= original_info.size) {
        fprintf(stderr,
                "[ffmpeg-gif] kept built-in GIF (%llu bytes; FFmpeg %llu bytes)\n",
                (unsigned long long)original_info.size,
                (unsigned long long)optimized_info.size);
        SDL_RemovePath(temp);
        free(temp);
        return true;
    }

    if (!SDL_RenamePath(temp, path)) {
        fprintf(stderr, "[ffmpeg-gif] could not install optimized GIF: %s\n",
                SDL_GetError());
        SDL_RemovePath(temp);
        free(temp);
        return false;
    }

    fprintf(stderr, "[ffmpeg-gif] reduced %llu -> %llu bytes\n",
            (unsigned long long)original_info.size,
            (unsigned long long)optimized_info.size);
    free(temp);
    return true;
#endif
}

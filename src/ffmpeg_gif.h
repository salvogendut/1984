/* ffmpeg_gif.h - optional FFmpeg optimization pass for animated GIFs. */
#pragma once

#include <stdbool.h>

/* Re-encode an existing GIF through a global palette and inter-frame
 * differences. The original is replaced only when FFmpeg succeeds and its
 * result is smaller. */
bool ffmpeg_gif_optimize(const char *path);

#ifdef HAVE_FFMPEG
#  define FFMPEG_GIF_SUPPORTED 1
#else
#  define FFMPEG_GIF_SUPPORTED 0
#endif

/* webmcap.c — WebM/VP9 capture via ffmpeg subprocess.
 *
 * popen() is POSIX-only; on Windows _popen exists with the same shape.
 * SDL3 supports Windows but the emulator's primary platforms are
 * Linux/BSD/macOS where popen works as expected. We do not handle
 * SIGPIPE specially: ffmpeg only closes its stdin when we ask it to
 * (via pclose), so a broken pipe here means ffmpeg itself crashed —
 * we treat that as end-of-recording.
 */
#define _POSIX_C_SOURCE 200809L
#include "webmcap.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <io.h>
#  define popen  _popen
#  define pclose _pclose
#endif

struct WebmCap {
    FILE *fp;
    int   in_w, in_h;
    int   frames;
    bool  broken;
};

WebmCap *webmcap_open(const char *path,
                      int in_w, int in_h,
                      int out_w, int out_h) {
#ifndef HAVE_FFMPEG
    (void)path; (void)in_w; (void)in_h; (void)out_w; (void)out_h;
    fprintf(stderr, "[webmcap] not compiled with ffmpeg support\n");
    return NULL;
#else
    if (!path || in_w <= 0 || in_h <= 0 || out_w <= 0 || out_h <= 0)
        return NULL;

    /* Ensure SIGPIPE doesn't kill us if ffmpeg dies mid-recording. */
    signal(SIGPIPE, SIG_IGN);

    /* ffmpeg invocation:
     *  -y                  overwrite output
     *  -f rawvideo         input is raw pixels (no container)
     *  -pixel_format bgra  SDL ARGB8888 == BGRA in little-endian byte order
     *  -video_size WxH     input dimensions
     *  -framerate 50       CPC native vsync
     *  -i -                stdin
     *  -vf scale=W:H:flags=neighbor   nearest-neighbour upscale (keep pixel art)
     *  -c:v libvpx-vp9     open, royalty-free, YouTube-native
     *  -b:v 2M             ~2 Mbit/s is plenty for 768x576 cartoon-flat content
     *  -row-mt 1           multithread the row encoder
     *  -loglevel error     keep our stderr clean
     */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "%s -y -loglevel error "
        "-f rawvideo -pixel_format bgra -video_size %dx%d -framerate 50 -i - "
        "-vf scale=%d:%d:flags=neighbor,format=yuv420p "
        "-c:v libvpx-vp9 -b:v 2M -row-mt 1 -deadline good -cpu-used 4 "
        "\"%s\"",
        FFMPEG_PATH, in_w, in_h, out_w, out_h, path);

    FILE *fp = popen(cmd, "w");
    if (!fp) {
        fprintf(stderr, "[webmcap] popen('%s'): failed\n", FFMPEG_PATH);
        return NULL;
    }
    WebmCap *w = calloc(1, sizeof(*w));
    if (!w) { pclose(fp); return NULL; }
    w->fp   = fp;
    w->in_w = in_w;
    w->in_h = in_h;
    fprintf(stderr, "[webmcap] recording %dx%d -> %dx%d VP9 -> %s\n",
            in_w, in_h, out_w, out_h, path);
    return w;
#endif
}

bool webmcap_frame(WebmCap *w, const uint32_t *pixels) {
    if (!w || !w->fp || !pixels || w->broken) return false;
    size_t n = (size_t)w->in_w * (size_t)w->in_h;
    size_t wr = fwrite(pixels, 4, n, w->fp);
    if (wr != n) {
        fprintf(stderr, "[webmcap] short write (%zu/%zu) — ffmpeg gone?\n",
                wr, n);
        w->broken = true;
        return false;
    }
    w->frames++;
    return true;
}

void webmcap_close(WebmCap *w) {
    if (!w) return;
    int n = w->frames;
    if (w->fp) {
        fflush(w->fp);
        int rc = pclose(w->fp);
        fprintf(stderr, "[webmcap] finished — %d frames, ffmpeg exit=%d\n",
                n, rc);
    }
    free(w);
}

int webmcap_frame_count(const WebmCap *w) { return w ? w->frames : 0; }

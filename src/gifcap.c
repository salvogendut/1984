/* gifcap.c — minimal GIF89a writer with in-tree LZW. See gifcap.h. */
#include "gifcap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GIFCAP_MAX_W   2048
#define GIFCAP_MAX_H   2048

/* Output sink: file mode (fp non-NULL) or growable memory buffer. */
typedef struct {
    FILE    *fp;
    uint8_t *buf;
    size_t   len, cap;
    bool     oom;
} GifOut;

static void go_write(GifOut *o, const void *p, size_t n) {
    if (o->fp) { fwrite(p, 1, n, o->fp); return; }
    if (o->oom) return;
    if (o->len + n > o->cap) {
        size_t cap = o->cap ? o->cap : 16384;
        while (cap < o->len + n) cap *= 2;
        uint8_t *nb = realloc(o->buf, cap);
        if (!nb) { o->oom = true; return; }
        o->buf = nb;
        o->cap = cap;
    }
    memcpy(o->buf + o->len, p, n);
    o->len += n;
}

static inline void go_putc(GifOut *o, int c) {
    uint8_t b = (uint8_t)c;
    go_write(o, &b, 1);
}

struct GifCap {
    GifOut  out;
    int     in_w, in_h;      /* input framebuffer size */
    int     w, h;            /* output (GIF) size, nearest-neighbour scaled */
    int     delay_cs;
    int     frames;
    /* Per-frame scratch — sized for the OUTPUT frame. */
    uint8_t *indices;        /* w*h palette indices */
    int     *row_src;        /* h entries: source row index for each out row */
    int     *col_src;        /* w entries: source col index for each out col */
    uint32_t palette[256];
    int      palette_size;
};

/* ---------- palette ---------- */

/* Hashtable mapping 0x00RRGGBB → palette index. Open addressing, ~1024
 * slots is plenty for ≤256 unique colours. */
#define PAL_HASH_N  1024
static int pal_find_or_insert(uint32_t *pal, int *pal_size,
                              int16_t *htab, uint32_t rgb) {
    uint32_t key = rgb | 0x01000000u;   /* never 0 — use 0 as empty */
    uint32_t h = (rgb * 0x9E3779B1u) >> 22;   /* fold to 10 bits */
    for (;;) {
        int16_t slot = htab[h];
        if (slot == -1) {
            if (*pal_size >= 256) return -1;
            int idx = (*pal_size)++;
            pal[idx] = rgb & 0x00FFFFFFu;
            htab[h] = (int16_t)idx;
            return idx;
        }
        if ((pal[slot] | 0x01000000u) == key) return slot;
        h = (h + 1) & (PAL_HASH_N - 1);
    }
}

/* Build per-frame palette + index buffer (with nearest-neighbour
 * scaling baked in). Returns true if losslessly mappable in ≤256
 * colours; false → caller must fall back. */
static bool build_palette(GifCap *g, const uint32_t *pixels) {
    int16_t htab[PAL_HASH_N];
    for (int i = 0; i < PAL_HASH_N; i++) htab[i] = -1;
    g->palette_size = 0;
    for (int y = 0; y < g->h; y++) {
        const uint32_t *src_row = pixels + g->row_src[y] * g->in_w;
        uint8_t *dst_row = g->indices + y * g->w;
        for (int x = 0; x < g->w; x++) {
            uint32_t rgb = src_row[g->col_src[x]] & 0x00FFFFFFu;
            int idx = pal_find_or_insert(g->palette, &g->palette_size, htab, rgb);
            if (idx < 0) return false;
            dst_row[x] = (uint8_t)idx;
        }
    }
    return true;
}

/* Fallback: RGB332 uniform quantisation. Always fits in 256 entries. */
static void build_palette_rgb332(GifCap *g, const uint32_t *pixels) {
    g->palette_size = 256;
    for (int i = 0; i < 256; i++) {
        int r3 = (i >> 5) & 0x7;
        int g3 = (i >> 2) & 0x7;
        int b2 = (i     ) & 0x3;
        int r = (r3 * 255) / 7;
        int gg = (g3 * 255) / 7;
        int b = (b2 * 255) / 3;
        g->palette[i] = ((uint32_t)r << 16) | ((uint32_t)gg << 8) | (uint32_t)b;
    }
    for (int y = 0; y < g->h; y++) {
        const uint32_t *src_row = pixels + g->row_src[y] * g->in_w;
        uint8_t *dst_row = g->indices + y * g->w;
        for (int x = 0; x < g->w; x++) {
            uint32_t rgb = src_row[g->col_src[x]];
            int r = (rgb >> 16) & 0xFF;
            int gg = (rgb >> 8) & 0xFF;
            int b = (rgb     ) & 0xFF;
            dst_row[x] = (uint8_t)(((r >> 5) << 5) | ((gg >> 5) << 2) | (b >> 6));
        }
    }
}

/* ---------- LZW encoder + sub-block packer ---------- */

typedef struct {
    GifOut *out;
    uint8_t buf[255];
    int     buf_len;
    uint32_t bit_buf;
    int     bit_cnt;
} LzwOut;

static void lzw_flush_block(LzwOut *o) {
    if (o->buf_len == 0) return;
    go_putc(o->out, o->buf_len);
    go_write(o->out, o->buf, (size_t)o->buf_len);
    o->buf_len = 0;
}

static void lzw_emit_code(LzwOut *o, uint32_t code, int code_size) {
    o->bit_buf |= (code << o->bit_cnt);
    o->bit_cnt += code_size;
    while (o->bit_cnt >= 8) {
        o->buf[o->buf_len++] = (uint8_t)(o->bit_buf & 0xFF);
        o->bit_buf >>= 8;
        o->bit_cnt -= 8;
        if (o->buf_len == 255) lzw_flush_block(o);
    }
}

static void lzw_finish(LzwOut *o) {
    if (o->bit_cnt > 0) {
        o->buf[o->buf_len++] = (uint8_t)(o->bit_buf & 0xFF);
        o->bit_buf = 0;
        o->bit_cnt = 0;
        if (o->buf_len == 255) lzw_flush_block(o);
    }
    lzw_flush_block(o);
    go_putc(o->out, 0);   /* end-of-sub-block-stream */
}

/* Dictionary: chained hash keyed on (prefix << 8) | byte → code.
 * 5021 is a small prime > 4096 (max LZW table size). */
#define LZW_HASH_N 5021
static void lzw_encode(GifOut *out, const uint8_t *data, int n, int min_code_size) {
    LzwOut o = { .out = out };
    go_putc(out, min_code_size);

    uint32_t hash_key[LZW_HASH_N];
    int16_t  hash_val[LZW_HASH_N];
    for (int i = 0; i < LZW_HASH_N; i++) { hash_key[i] = 0xFFFFFFFFu; hash_val[i] = 0; }

    int clear_code = 1 << min_code_size;
    int end_code   = clear_code + 1;
    int next_code  = clear_code + 2;
    int code_size  = min_code_size + 1;
    int max_code   = (1 << code_size);

    lzw_emit_code(&o, (uint32_t)clear_code, code_size);
    if (n == 0) { lzw_emit_code(&o, (uint32_t)end_code, code_size); lzw_finish(&o); return; }

    int prefix = data[0];
    for (int i = 1; i < n; i++) {
        int k = data[i];
        uint32_t key = ((uint32_t)prefix << 8) | (uint32_t)k;
        uint32_t h = (key * 0x9E3779B1u) % LZW_HASH_N;
        while (hash_key[h] != 0xFFFFFFFFu && hash_key[h] != key) {
            h++;
            if (h >= LZW_HASH_N) h = 0;
        }
        if (hash_key[h] == key) {
            prefix = hash_val[h];
            continue;
        }
        lzw_emit_code(&o, (uint32_t)prefix, code_size);
        if (next_code < 4096) {
            hash_key[h] = key;
            hash_val[h] = (int16_t)next_code;
            next_code++;
            if (next_code > max_code && code_size < 12) {
                code_size++;
                max_code = 1 << code_size;
            }
        } else {
            lzw_emit_code(&o, (uint32_t)clear_code, code_size);
            for (int j = 0; j < LZW_HASH_N; j++) hash_key[j] = 0xFFFFFFFFu;
            next_code = clear_code + 2;
            code_size = min_code_size + 1;
            max_code  = 1 << code_size;
        }
        prefix = k;
    }
    lzw_emit_code(&o, (uint32_t)prefix, code_size);
    lzw_emit_code(&o, (uint32_t)end_code, code_size);
    lzw_finish(&o);
}

/* ---------- GIF header / per-frame block ---------- */

static void write_u16(GifOut *o, uint16_t v) { go_putc(o, v & 0xFF); go_putc(o, (v >> 8) & 0xFF); }

static void write_header(GifCap *g, bool loop) {
    GifOut *o = &g->out;
    go_write(o, "GIF89a", 6);
    /* Logical Screen Descriptor */
    write_u16(o, (uint16_t)g->w);
    write_u16(o, (uint16_t)g->h);
    go_putc(o, 0x00);    /* packed: no global colour table */
    go_putc(o, 0x00);    /* background index */
    go_putc(o, 0x00);    /* pixel aspect ratio */
    if (loop) {
        /* NETSCAPE2.0 application extension — loop forever. Only
         * meaningful for multi-frame files; single-frame stream
         * images omit it. */
        go_putc(o, 0x21); go_putc(o, 0xFF); go_putc(o, 0x0B);
        go_write(o, "NETSCAPE2.0", 11);
        go_putc(o, 0x03); go_putc(o, 0x01);
        write_u16(o, 0);   /* 0 = loop forever */
        go_putc(o, 0x00);
    }
}

static int log2_ceil(int n) {
    int v = 1, k = 0;
    while (v < n) { v <<= 1; k++; }
    if (k < 1) k = 1;
    return k;
}

static void write_frame(GifCap *g) {
    GifOut *o = &g->out;
    int lct_bits = log2_ceil(g->palette_size);
    if (lct_bits < 2) lct_bits = 2;            /* GIF requires at least 4 entries */
    int lct_count = 1 << lct_bits;

    /* Graphic Control Extension */
    go_putc(o, 0x21); go_putc(o, 0xF9); go_putc(o, 0x04);
    go_putc(o, 0x00);                           /* no transparency, no disposal */
    write_u16(o, (uint16_t)g->delay_cs);
    go_putc(o, 0x00); go_putc(o, 0x00);

    /* Image Descriptor */
    go_putc(o, 0x2C);
    write_u16(o, 0); write_u16(o, 0);
    write_u16(o, (uint16_t)g->w);
    write_u16(o, (uint16_t)g->h);
    /* packed: LCT=1, interlace=0, sort=0, reserved=0, size = lct_bits-1 */
    go_putc(o, 0x80 | (lct_bits - 1));

    /* Local colour table — pad to power-of-two size. */
    for (int i = 0; i < lct_count; i++) {
        uint32_t c = (i < g->palette_size) ? g->palette[i] : 0;
        go_putc(o, (c >> 16) & 0xFF);
        go_putc(o, (c >> 8)  & 0xFF);
        go_putc(o, (c)       & 0xFF);
    }

    /* LZW image data. Min code size must be at least 2. */
    int min_code_size = lct_bits;
    if (min_code_size < 2) min_code_size = 2;
    lzw_encode(o, g->indices, g->w * g->h, min_code_size);
}

/* ---------- public ---------- */

static GifCap *gifcap_alloc(int in_w, int in_h, int out_w, int out_h,
                            int frame_delay_cs) {
    if (in_w <= 0 || in_h <= 0 || out_w <= 0 || out_h <= 0 ||
        out_w > GIFCAP_MAX_W || out_h > GIFCAP_MAX_H) return NULL;
    if (frame_delay_cs < 1) frame_delay_cs = 1;
    GifCap *g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->in_w = in_w;
    g->in_h = in_h;
    g->w = out_w;
    g->h = out_h;
    g->delay_cs = frame_delay_cs;
    g->indices = malloc((size_t)out_w * (size_t)out_h);
    g->row_src = malloc(sizeof(int) * out_h);
    g->col_src = malloc(sizeof(int) * out_w);
    if (!g->indices || !g->row_src || !g->col_src) {
        free(g->indices); free(g->row_src); free(g->col_src);
        free(g); return NULL;
    }
    /* Precompute nearest-neighbour source coordinates. */
    for (int y = 0; y < out_h; y++) {
        int s = (y * in_h) / out_h;
        if (s >= in_h) s = in_h - 1;
        g->row_src[y] = s;
    }
    for (int x = 0; x < out_w; x++) {
        int s = (x * in_w) / out_w;
        if (s >= in_w) s = in_w - 1;
        g->col_src[x] = s;
    }
    return g;
}

GifCap *gifcap_open(const char *path,
                    int in_w, int in_h, int out_w, int out_h,
                    int frame_delay_cs) {
    if (!path) return NULL;
    GifCap *g = gifcap_alloc(in_w, in_h, out_w, out_h, frame_delay_cs);
    if (!g) return NULL;
    g->out.fp = fopen(path, "wb");
    if (!g->out.fp) { gifcap_close(g); return NULL; }
    write_header(g, true);
    return g;
}

GifCap *gifcap_open_mem(int in_w, int in_h, int out_w, int out_h,
                        int frame_delay_cs) {
    return gifcap_alloc(in_w, in_h, out_w, out_h, frame_delay_cs);
}

const uint8_t *gifcap_encode_single(GifCap *g, const uint32_t *pixels,
                                    size_t *out_len) {
    if (!g || !pixels || !out_len || g->out.fp) return NULL;
    g->out.len = 0;
    g->out.oom = false;
    write_header(g, false);
    if (!build_palette(g, pixels))
        build_palette_rgb332(g, pixels);
    write_frame(g);
    go_putc(&g->out, 0x3B);    /* trailer — complete standalone GIF */
    g->frames++;
    if (g->out.oom) return NULL;
    *out_len = g->out.len;
    return g->out.buf;
}

bool gifcap_frame(GifCap *g, const uint32_t *pixels) {
    if (!g || !pixels) return false;
    if (!build_palette(g, pixels))
        build_palette_rgb332(g, pixels);
    write_frame(g);
    g->frames++;
    return true;
}

void gifcap_close(GifCap *g) {
    if (!g) return;
    if (g->out.fp) {
        fputc(0x3B, g->out.fp);    /* trailer */
        fclose(g->out.fp);
    }
    free(g->out.buf);
    free(g->indices);
    free(g->row_src);
    free(g->col_src);
    free(g);
}

int gifcap_frame_count(const GifCap *g) { return g ? g->frames : 0; }

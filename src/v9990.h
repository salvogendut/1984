#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "types.h"

#define V9990_VRAM_SIZE 0x80000u
#define V9990_REGISTER_COUNT 64u
#define V9990_PALETTE_BYTES 256u
#define V9990_MAX_WIDTH 1024u
#define V9990_MAX_HEIGHT 290u

typedef enum {
    V9990_MODE_P1 = 0,
    V9990_MODE_P2,
    V9990_MODE_B0,
    V9990_MODE_B1,
    V9990_MODE_B2,
    V9990_MODE_B3,
    V9990_MODE_B4,
    V9990_MODE_B7
} V9990DisplayMode;

typedef struct V9990 {
    u8 *vram;
    u32 *pixels;
    u8 registers[V9990_REGISTER_COUNT];
    u8 palette[V9990_PALETTE_BYTES];
    u8 register_select;
    u8 status;
    u8 pending_irqs;
    u8 command_status;
    u8 command_data;
    u8 command_partial;
    u8 read_buffer;
    u32 read_address;
    u32 write_address;
    u16 command_sx;
    u16 command_sy;
    u16 command_dx;
    u16 command_dy;
    u16 command_nx;
    u16 command_ny;
    u16 command_remaining_x;
    u16 command_remaining_y;
    u16 command_line_start_x;
    bool enabled;
    bool system_reset;
    bool irq;
    bool command_cpu_write;
    bool command_cpu_read;
    bool command_end_after_read;
    bool command_high_byte;
    unsigned render_width;
    unsigned render_height;
    unsigned frame_cycles;
    unsigned frame_cycle;
    unsigned frame_scanlines;
} V9990;

void v9990_init(V9990 *v9990);
void v9990_destroy(V9990 *v9990);
int  v9990_set_enabled(V9990 *v9990, bool enabled);
void v9990_reset(V9990 *v9990);

bool v9990_io_read(V9990 *v9990, u16 port, u8 *value);
bool v9990_io_write(V9990 *v9990, u16 port, u8 value);

void v9990_begin_frame(V9990 *v9990, unsigned frame_cycles);
void v9990_advance(V9990 *v9990, unsigned cycles);
void v9990_end_frame(V9990 *v9990);
void v9990_render(V9990 *v9990);

V9990DisplayMode v9990_display_mode(const V9990 *v9990);
const char *v9990_mode_name(const V9990 *v9990);
bool v9990_display_active(const V9990 *v9990);

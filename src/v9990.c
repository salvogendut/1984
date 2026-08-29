#include "v9990.h"

#include <stdlib.h>
#include <string.h>

enum {
    V9990_PORT_VRAM = 0,
    V9990_PORT_PALETTE,
    V9990_PORT_COMMAND,
    V9990_PORT_REGISTER_DATA,
    V9990_PORT_REGISTER_SELECT,
    V9990_PORT_STATUS,
    V9990_PORT_INTERRUPT,
    V9990_PORT_SYSTEM
};

enum {
    V9990_REG_SCREEN_MODE_0 = 6,
    V9990_REG_SCREEN_MODE_1,
    V9990_REG_CONTROL,
    V9990_REG_INTERRUPT_ENABLE,
    V9990_REG_PALETTE_CONTROL = 13,
    V9990_REG_PALETTE_POINTER,
    V9990_REG_BACKDROP,
    V9990_REG_SCROLL_AY0 = 17,
    V9990_REG_SCROLL_AY1,
    V9990_REG_SCROLL_AX0,
    V9990_REG_SCROLL_AX1,
    V9990_REG_SCROLL_BY0,
    V9990_REG_SCROLL_BY1,
    V9990_REG_SCROLL_BX0,
    V9990_REG_SCROLL_BX1,
    V9990_REG_SPRITE_PATTERN,
    V9990_REG_PRIORITY = 27,
    V9990_REG_SPRITE_PALETTE,
    V9990_REG_COMMAND = 52
};

enum {
    V9990_IRQ_VERTICAL = 0x01,
    V9990_IRQ_HORIZONTAL = 0x02,
    V9990_IRQ_COMMAND = 0x04,
    V9990_STATUS_TR = 0x80,
    V9990_STATUS_BD = 0x10,
    V9990_STATUS_CE = 0x01
};

static const u8 register_access[V9990_REGISTER_COUNT] = {
    2, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 2, 2, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 2, 0, 0, 0,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

static const u8 register_write_mask[32] = {
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0x87, 0xff, 0x83, 0x0f, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xdf, 0x07, 0xff, 0xff, 0xc1, 0x07,
    0x3f, 0xcf, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

static unsigned transform_bx(unsigned address) {
    address &= V9990_VRAM_SIZE - 1;
    return ((address & 1u) << 18) | ((address & 0x7fffeu) >> 1);
}

static unsigned transform_p2(unsigned address) {
    address &= V9990_VRAM_SIZE - 1;
    if (address < 0x78000)
        return transform_bx(address);
    if (address < 0x7c000)
        return address - 0x3c000;
    return address;
}

V9990DisplayMode v9990_display_mode(const V9990 *v9990) {
    unsigned mode;

    if (!v9990)
        return V9990_MODE_P1;
    mode = v9990->registers[V9990_REG_SCREEN_MODE_0];
    switch (mode & 0xc0) {
        case 0x00: return V9990_MODE_P1;
        case 0x40: return V9990_MODE_P2;
        case 0x80:
            if (v9990->status & 0x04) {
                switch (mode & 0x30) {
                    case 0x00: return V9990_MODE_B0;
                    case 0x10: return V9990_MODE_B2;
                    default:   return V9990_MODE_B4;
                }
            }
            switch (mode & 0x30) {
                case 0x00: return V9990_MODE_B1;
                case 0x10: return V9990_MODE_B3;
                default:   return V9990_MODE_B7;
            }
        default: return V9990_MODE_P1;
    }
}

const char *v9990_mode_name(const V9990 *v9990) {
    switch (v9990_display_mode(v9990)) {
        case V9990_MODE_P1: return "P1";
        case V9990_MODE_P2: return "P2";
        case V9990_MODE_B0: return "B0";
        case V9990_MODE_B1: return "B1";
        case V9990_MODE_B2: return "B2";
        case V9990_MODE_B3: return "B3";
        case V9990_MODE_B4: return "B4";
        case V9990_MODE_B7: return "B7";
    }
    return "P1";
}

bool v9990_display_active(const V9990 *v9990) {
    return v9990 && v9990->enabled && !v9990->system_reset &&
           (v9990->registers[V9990_REG_CONTROL] & 0x80) != 0;
}

static unsigned map_cpu_address(const V9990 *v9990,
                                unsigned address) {
    switch (v9990_display_mode(v9990)) {
        case V9990_MODE_P1:
            return address & (V9990_VRAM_SIZE - 1);
        case V9990_MODE_P2:
            return transform_p2(address);
        default:
            return transform_bx(address);
    }
}

static u8 read_direct(const V9990 *v9990, unsigned address) {
    return v9990->vram[address & (V9990_VRAM_SIZE - 1)];
}

static void write_direct(V9990 *v9990, unsigned address, u8 value) {
    v9990->vram[address & (V9990_VRAM_SIZE - 1)] = value;
}

static u8 read_bx(const V9990 *v9990, unsigned address) {
    return read_direct(v9990, transform_bx(address));
}

static u8 read_p2(const V9990 *v9990, unsigned address) {
    return read_direct(v9990, transform_p2(address));
}

static unsigned register_address(const V9990 *v9990, unsigned base) {
    return (unsigned)v9990->registers[base] |
           ((unsigned)v9990->registers[base + 1] << 8) |
           (((unsigned)v9990->registers[base + 2] & 7u) << 16);
}

static void set_register_address(V9990 *v9990, unsigned base,
                                 unsigned address) {
    address &= V9990_VRAM_SIZE - 1;
    v9990->registers[base] = (u8)address;
    v9990->registers[base + 1] = (u8)(address >> 8);
    v9990->registers[base + 2] =
        (u8)((v9990->registers[base + 2] & 0x80) |
             ((address >> 16) & 7));
}

static void update_irq(V9990 *v9990) {
    v9990->irq =
        (v9990->pending_irqs &
         v9990->registers[V9990_REG_INTERRUPT_ENABLE]) != 0;
}

static void finish_command(V9990 *v9990) {
    v9990->command_cpu_write = false;
    v9990->command_cpu_read = false;
    v9990->command_end_after_read = false;
    v9990->command_high_byte = false;
    v9990->command_status &= (u8)~(V9990_STATUS_CE | V9990_STATUS_TR);
    v9990->pending_irqs |= V9990_IRQ_COMMAND;
    update_irq(v9990);
}

static unsigned image_width(const V9990 *v9990) {
    return 256u << ((v9990->registers[V9990_REG_SCREEN_MODE_0] >> 2) & 3);
}

static unsigned bits_per_pixel(const V9990 *v9990) {
    V9990DisplayMode mode = v9990_display_mode(v9990);

    if (mode == V9990_MODE_P1 || mode == V9990_MODE_P2)
        return 4;
    return 2u << (v9990->registers[V9990_REG_SCREEN_MODE_0] & 3);
}

static unsigned pixel_address(const V9990 *v9990,
                              unsigned x, unsigned y) {
    unsigned width = image_width(v9990);
    unsigned bpp = bits_per_pixel(v9990);
    unsigned logical;

    if (bpp == 16)
        return ((x & (width - 1)) + y * width) & 0x3ffff;
    logical = ((x * bpp / 8) & (width * bpp / 8 - 1)) +
              y * (width * bpp / 8);
    if (v9990_display_mode(v9990) == V9990_MODE_P1) {
        unsigned physical = logical & 0x3ffff;
        if (x & 0x200)
            physical |= 0x40000;
        return physical;
    }
    if (v9990_display_mode(v9990) == V9990_MODE_P2)
        return transform_p2(logical);
    return transform_bx(logical);
}

static u16 point(const V9990 *v9990, unsigned x, unsigned y) {
    unsigned bpp = bits_per_pixel(v9990);
    unsigned address = pixel_address(v9990, x, y);
    u8 data;

    if (bpp == 16)
        return (u16)(read_direct(v9990, address) |
                     ((u16)read_direct(v9990, address + 0x40000) << 8));
    data = read_direct(v9990, address);
    if (bpp == 8)
        return data;
    if (bpp == 4)
        return (u16)((data >> (4 * (1 - (x & 1)))) & 0x0f);
    return (u16)((data >> (2 * (3 - (x & 3)))) & 3);
}

static u16 logical_operation(u16 source, u16 destination,
                             unsigned bits, u8 operation) {
    u16 result = 0;
    u16 mask = bits == 16 ? 0xffff : (u16)((1u << bits) - 1);
    unsigned truth = operation & 0x0f;

    source &= mask;
    destination &= mask;
    if ((operation & 0x10) && source == 0)
        return destination;
    for (unsigned bit = 0; bit < bits; ++bit) {
        unsigned s = (source >> bit) & 1;
        unsigned d = (destination >> bit) & 1;
        if ((truth >> (s * 2 + d)) & 1)
            result |= (u16)(1u << bit);
    }
    return result & mask;
}

static void pset(V9990 *v9990, unsigned x, unsigned y, u16 colour) {
    unsigned bpp = bits_per_pixel(v9990);
    unsigned address = pixel_address(v9990, x, y);
    u16 destination = point(v9990, x, y);
    u16 write_mask = (u16)(v9990->registers[46] |
                           ((u16)v9990->registers[47] << 8));
    u16 result = logical_operation(
        colour, destination, bpp, v9990->registers[45]);

    if (bpp == 16) {
        result = (u16)((destination & ~write_mask) |
                       (result & write_mask));
        write_direct(v9990, address, (u8)result);
        write_direct(v9990, address + 0x40000, (u8)(result >> 8));
    } else {
        u8 data = read_direct(v9990, address);
        u8 pixel_mask = bpp == 8 ? 0xff :
            bpp == 4 ? (u8)(0xf0 >> (4 * (x & 1))) :
                       (u8)(0xc0 >> (2 * (x & 3)));
        u8 plane_mask = (address & 0x40000)
                      ? (u8)(write_mask >> 8) : (u8)write_mask;
        unsigned shift = bpp == 8 ? 0 :
            bpp == 4 ? 4 * (1 - (x & 1)) : 2 * (3 - (x & 3));
        u8 shifted = (u8)(result << shift);
        u8 mask = pixel_mask & plane_mask;

        write_direct(v9990, address,
                     (u8)((data & ~mask) | (shifted & mask)));
    }
}

static unsigned wrapped_nx(const V9990 *v9990) {
    return v9990->command_nx ? v9990->command_nx : 2048;
}

static unsigned wrapped_ny(const V9990 *v9990) {
    return v9990->command_ny ? v9990->command_ny : 4096;
}

static bool step_command_position(V9990 *v9990, bool source) {
    int dx = (v9990->registers[44] & 0x04) ? -1 : 1;
    int dy = (v9990->registers[44] & 0x08) ? -1 : 1;
    u16 *x = source ? &v9990->command_sx : &v9990->command_dx;
    u16 *y = source ? &v9990->command_sy : &v9990->command_dy;

    *x = (u16)(*x + dx);
    if (--v9990->command_remaining_x)
        return false;
    *x = v9990->command_line_start_x;
    *y = (u16)(*y + dy);
    v9990->command_remaining_x = (u16)wrapped_nx(v9990);
    return !--v9990->command_remaining_y;
}

static void advance_command_position(V9990 *v9990, bool source) {
    if (step_command_position(v9990, source))
        finish_command(v9990);
}

static void load_command_registers(V9990 *v9990) {
    v9990->command_sx = (u16)(v9990->registers[32] |
        ((u16)(v9990->registers[33] & 7) << 8));
    v9990->command_sy = (u16)(v9990->registers[34] |
        ((u16)(v9990->registers[35] & 15) << 8));
    v9990->command_dx = (u16)(v9990->registers[36] |
        ((u16)(v9990->registers[37] & 7) << 8));
    v9990->command_dy = (u16)(v9990->registers[38] |
        ((u16)(v9990->registers[39] & 15) << 8));
    v9990->command_nx = (u16)(v9990->registers[40] |
        ((u16)(v9990->registers[41] & 15) << 8));
    v9990->command_ny = (u16)(v9990->registers[42] |
        ((u16)(v9990->registers[43] & 15) << 8));
    v9990->command_remaining_x = (u16)wrapped_nx(v9990);
    v9990->command_remaining_y = (u16)wrapped_ny(v9990);
}

static void run_immediate_command(V9990 *v9990, u8 command) {
    unsigned nx = wrapped_nx(v9990);
    unsigned ny = wrapped_ny(v9990);
    unsigned limit = nx * ny;
    int dix = (v9990->registers[44] & 0x04) ? -1 : 1;
    int diy = (v9990->registers[44] & 0x08) ? -1 : 1;
    u16 fg = (u16)(v9990->registers[48] |
                   ((u16)v9990->registers[49] << 8));

    if (limit > V9990_VRAM_SIZE * 8u)
        limit = V9990_VRAM_SIZE * 8u;
    switch (command) {
        case 0x02: /* LMMV */
            for (unsigned y = 0; y < ny && limit; ++y) {
                for (unsigned x = 0; x < nx && limit; ++x, --limit)
                    pset(v9990,
                         (u16)(v9990->command_dx + (int)x * dix),
                         (u16)(v9990->command_dy + (int)y * diy), fg);
            }
            finish_command(v9990);
            break;
        case 0x04: /* LMMM */
            for (unsigned y = 0; y < ny && limit; ++y) {
                for (unsigned x = 0; x < nx && limit; ++x, --limit) {
                    u16 colour = point(
                        v9990,
                        (u16)(v9990->command_sx + (int)x * dix),
                        (u16)(v9990->command_sy + (int)y * diy));
                    pset(v9990,
                         (u16)(v9990->command_dx + (int)x * dix),
                         (u16)(v9990->command_dy + (int)y * diy), colour);
                }
            }
            finish_command(v9990);
            break;
        case 0x0d: /* POINT */
            v9990->command_data = (u8)point(
                v9990, v9990->command_sx, v9990->command_sy);
            v9990->command_cpu_read = true;
            v9990->command_end_after_read = true;
            v9990->command_status |= V9990_STATUS_TR;
            break;
        case 0x0e: /* PSET */
            pset(v9990, v9990->command_dx, v9990->command_dy, fg);
            finish_command(v9990);
            break;
        case 0x0f: /* ADVN */
            finish_command(v9990);
            break;
        default:
            finish_command(v9990);
            break;
    }
}

static void prepare_lmcm_data(V9990 *v9990) {
    unsigned bpp = bits_per_pixel(v9990);
    unsigned pixels = bpp < 8 ? 8 / bpp : 1;
    u8 data = 0;

    if (bpp == 16) {
        u16 colour = point(
            v9990, v9990->command_sx, v9990->command_sy);

        v9990->command_data = (u8)colour;
        v9990->command_partial = (u8)(colour >> 8);
        v9990->command_high_byte = false;
        v9990->command_status |= V9990_STATUS_TR;
        return;
    }
    for (unsigned i = 0; i < pixels; ++i) {
        u16 colour = point(
            v9990, v9990->command_sx, v9990->command_sy);

        if (bpp < 8)
            data |= (u8)(colour << (8 - bpp * (i + 1)));
        else
            data = (u8)colour;
        if (step_command_position(v9990, true)) {
            v9990->command_end_after_read = true;
            break;
        }
    }
    v9990->command_data = data;
    v9990->command_status |= V9990_STATUS_TR;
}

static void consume_lmcm_data(V9990 *v9990) {
    unsigned bpp = bits_per_pixel(v9990);

    v9990->command_status &= (u8)~V9990_STATUS_TR;
    if (bpp == 16) {
        if (!v9990->command_high_byte) {
            v9990->command_data = v9990->command_partial;
            v9990->command_high_byte = true;
            v9990->command_status |= V9990_STATUS_TR;
            return;
        }
        v9990->command_high_byte = false;
        if (step_command_position(v9990, true)) {
            finish_command(v9990);
            return;
        }
        prepare_lmcm_data(v9990);
        return;
    }
    if (v9990->command_end_after_read)
        finish_command(v9990);
    else
        prepare_lmcm_data(v9990);
}

static void start_command(V9990 *v9990, u8 value) {
    u8 command = value >> 4;

    load_command_registers(v9990);
    v9990->command_status = V9990_STATUS_CE;
    v9990->command_cpu_write = false;
    v9990->command_cpu_read = false;
    v9990->command_end_after_read = false;
    v9990->command_high_byte = false;
    if (command == 0) {
        finish_command(v9990);
    } else if (command == 1 || command == 5) {
        v9990->command_cpu_write = true;
        v9990->command_line_start_x = v9990->command_dx;
        v9990->command_status |= V9990_STATUS_TR;
    } else if (command == 3) {
        v9990->command_cpu_read = true;
        v9990->command_line_start_x = v9990->command_sx;
        prepare_lmcm_data(v9990);
    } else {
        run_immediate_command(v9990, command);
    }
}

static void write_register(V9990 *v9990, unsigned reg, u8 value) {
    if (reg >= V9990_REGISTER_COUNT || !(register_access[reg] & 2))
        return;
    if (reg < 32)
        value &= register_write_mask[reg];
    v9990->registers[reg] = value;
    if (reg == 2) {
        v9990->write_address = register_address(v9990, 0);
    } else if (reg == 5) {
        v9990->read_address = register_address(v9990, 3);
        v9990->read_buffer = read_direct(
            v9990, map_cpu_address(v9990, v9990->read_address));
    } else if (reg == V9990_REG_INTERRUPT_ENABLE) {
        update_irq(v9990);
    } else if (reg == V9990_REG_COMMAND) {
        start_command(v9990, value);
    }
}

static u8 read_register(const V9990 *v9990, unsigned reg) {
    if (v9990->system_reset || reg >= V9990_REGISTER_COUNT ||
        !(register_access[reg] & 1))
        return 0xff;
    return v9990->registers[reg];
}

static u32 rgb5(unsigned red, unsigned green, unsigned blue) {
    red = red * 255 / 31;
    green = green * 255 / 31;
    blue = blue * 255 / 31;
    return (red << 16) | (green << 8) | blue;
}

static u32 palette_colour(const V9990 *v9990, unsigned index) {
    index &= 63;
    return rgb5(v9990->palette[index * 4] & 31,
                v9990->palette[index * 4 + 1] & 31,
                v9990->palette[index * 4 + 2] & 31);
}

static u32 colour_256(unsigned index) {
    static const u8 map_rg[8] = {0, 4, 9, 13, 18, 22, 27, 31};
    static const u8 map_b[4] = {0, 11, 21, 31};
    return rgb5(map_rg[(index >> 2) & 7],
                map_rg[(index >> 5) & 7], map_b[index & 3]);
}

static u32 bitmap_colour(const V9990 *v9990,
                         unsigned x, unsigned y, bool high_resolution) {
    unsigned bpp = bits_per_pixel(v9990);
    unsigned width = image_width(v9990);
    unsigned logical;
    unsigned palette_offset =
        v9990->registers[V9990_REG_PALETTE_CONTROL] & 15;
    u8 data;

    if (bpp == 16) {
        unsigned address = (x & (width - 1)) + y * width;
        unsigned grb = read_direct(v9990, address) |
            ((unsigned)read_direct(v9990, address + 0x40000) << 8);
        return rgb5((grb >> 5) & 31, (grb >> 10) & 31, grb & 31);
    }
    logical = (x * bpp / 8 & (width * bpp / 8 - 1)) +
              y * (width * bpp / 8);
    data = read_bx(v9990, logical);
    if (bpp == 8) {
        unsigned colour_mode =
            v9990->registers[V9990_REG_PALETTE_CONTROL] & 0xc0;
        if (colour_mode == 0x40)
            return colour_256(data);
        if (colour_mode == 0x00)
            return palette_colour(v9990, data & 63);
        /* YJK/YUV use four-pixel groups; direct RGB is a useful fallback
         * until the neighbouring chroma samples are available here. */
        return rgb5((data >> 3) & 31, (data >> 3) & 31,
                    (data >> 3) & 31);
    }
    if (bpp == 4) {
        unsigned index = (x & 1) ? data & 15 : data >> 4;
        unsigned offset = high_resolution
                        ? ((palette_offset & 4) << 2) |
                          ((x & 1) ? 32 : 0)
                        : ((palette_offset & 12) << 2);
        return palette_colour(v9990, offset + index);
    }
    {
        unsigned shift = 6 - 2 * (x & 3);
        unsigned index = (data >> shift) & 3;
        unsigned offset = high_resolution
                        ? ((palette_offset & 7) << 2) |
                          ((x & 1) ? 32 : 0)
                        : palette_offset << 2;
        return palette_colour(v9990, offset + index);
    }
}

static unsigned pattern_number(const V9990 *v9990, bool p2,
                               unsigned name_table,
                               unsigned x, unsigned y) {
    unsigned name_chars = p2 ? 128 : 64;
    unsigned address = name_table +
        ((y / 8) * name_chars + x / 8) * 2;
    u8 low = p2 ? read_direct(v9990, address) : read_direct(v9990, address);
    u8 high = p2 ? read_direct(v9990, address + 1) : read_direct(v9990, address + 1);
    return (low | ((unsigned)high << 8)) & 0x1fff;
}

static unsigned pattern_pixel(const V9990 *v9990, bool p2,
                              unsigned name_table, unsigned pattern_base,
                              unsigned x, unsigned y) {
    unsigned pattern_chars = p2 ? 64 : 32;
    unsigned pattern = pattern_number(v9990, p2, name_table, x, y);
    unsigned pitch = pattern_chars * 8 * 4;
    unsigned address = pattern_base +
        (pattern / pattern_chars) * pitch + (y & 7) * pattern_chars * 4 +
        (pattern % pattern_chars) * 4 + ((x & 7) / 2);
    u8 data = p2 ? read_p2(v9990, address) : read_direct(v9990, address);

    return (x & 1) ? data & 15 : data >> 4;
}

static u32 render_p1_pixel(const V9990 *v9990,
                           unsigned x, unsigned y,
                           bool *foreground) {
    unsigned ax = (x + v9990->registers[V9990_REG_SCROLL_AX0] +
        8u * v9990->registers[V9990_REG_SCROLL_AX1]) & 511;
    unsigned ay = (y + v9990->registers[V9990_REG_SCROLL_AY0] +
        256u * v9990->registers[V9990_REG_SCROLL_AY1]) & 511;
    unsigned bx = (x + v9990->registers[V9990_REG_SCROLL_BX0] +
        8u * v9990->registers[V9990_REG_SCROLL_BX1]) & 511;
    unsigned by = (y + v9990->registers[V9990_REG_SCROLL_BY0] +
        256u * v9990->registers[V9990_REG_SCROLL_BY1]) & 511;
    unsigned a = pattern_pixel(v9990, false, 0x7c000, 0, ax, ay);
    unsigned b = pattern_pixel(v9990, false, 0x7e000, 0x40000, bx, by);
    unsigned priority_x = v9990->registers[V9990_REG_PRIORITY] & 3;
    unsigned priority_y = v9990->registers[V9990_REG_PRIORITY] & 12;
    unsigned offset = v9990->registers[V9990_REG_PALETTE_CONTROL] & 15;
    bool a_front;

    *foreground = false;
    priority_x = priority_x ? priority_x << 6 : 256;
    priority_y = priority_y ? priority_y << 4 : 256;
    a_front = y < priority_y && x < priority_x;
    if (a_front && a) {
        *foreground = true;
        return palette_colour(v9990, ((offset & 3) << 4) + a);
    }
    if (!a_front && b) {
        *foreground = true;
        return palette_colour(v9990, ((offset & 12) << 2) + b);
    }
    if (a_front && b)
        return palette_colour(v9990, ((offset & 12) << 2) + b);
    if (!a_front && a)
        return palette_colour(v9990, ((offset & 3) << 4) + a);
    return palette_colour(v9990, v9990->registers[V9990_REG_BACKDROP]);
}

static u32 render_p2_pixel(const V9990 *v9990,
                           unsigned x, unsigned y,
                           bool *foreground) {
    unsigned sx = (x + v9990->registers[V9990_REG_SCROLL_AX0] +
        8u * v9990->registers[V9990_REG_SCROLL_AX1]) & 1023;
    unsigned sy = (y + v9990->registers[V9990_REG_SCROLL_AY0] +
        256u * v9990->registers[V9990_REG_SCROLL_AY1]) & 511;
    unsigned pixel = pattern_pixel(v9990, true, 0x7c000, 0, sx, sy);
    unsigned offset = v9990->registers[V9990_REG_PALETTE_CONTROL] & 15;
    unsigned palette = ((sx & 2) ? (offset & 12) << 2
                                 : (offset & 3) << 4);

    *foreground = pixel != 0;
    return pixel ? palette_colour(v9990, palette + pixel)
                 : palette_colour(
                       v9990, v9990->registers[V9990_REG_BACKDROP]);
}

static u8 pattern_mode_read(const V9990 *v9990,
                            V9990DisplayMode mode, unsigned address,
                            bool pattern) {
    if (mode == V9990_MODE_P2 && pattern)
        return read_p2(v9990, address);
    return read_direct(v9990, address);
}

static unsigned sprite_pattern_offset(V9990DisplayMode mode,
                                      u8 number, unsigned line) {
    if (mode == V9990_MODE_P2)
        return 256u * (((number & 0xe0u) >> 1) + line) +
               8u * (number & 0x1fu);
    return 128u * ((number & 0xf0u) + line) +
           8u * (number & 0x0fu);
}

static void render_pattern_sprites(const V9990 *v9990,
                                   V9990DisplayMode mode,
                                   unsigned y, unsigned width,
                                   u32 *pixels, u8 *coverage) {
    int visible[16];
    unsigned visible_count = 0;
    unsigned line_count = 0;
    unsigned pattern_base = mode == V9990_MODE_P2
                          ? (unsigned)(v9990->registers[
                                V9990_REG_SPRITE_PATTERN] & 0x0f) << 15
                          : (unsigned)(v9990->registers[
                                V9990_REG_SPRITE_PATTERN] & 0x0e) << 14;

    if (v9990->registers[V9990_REG_CONTROL] & 0x40)
        return;

    for (unsigned sprite = 0; sprite < 125 && line_count < 16; ++sprite) {
        unsigned address = 0x3fe00 + 4 * sprite;
        u8 sprite_y = pattern_mode_read(
            v9990, mode, address, false) + 1;
        unsigned line = (u8)(y - sprite_y);

        if (line >= 16)
            continue;
        ++line_count;
        if (!(pattern_mode_read(v9990, mode, address + 3, false) & 0x10))
            visible[visible_count++] = (int)sprite;
    }

    for (unsigned i = 0; i < visible_count; ++i) {
        unsigned address = 0x3fe00 + 4 * (unsigned)visible[i];
        u8 sprite_y = pattern_mode_read(v9990, mode, address, false);
        u8 number = pattern_mode_read(v9990, mode, address + 1, false);
        u8 attr = pattern_mode_read(v9990, mode, address + 3, false);
        unsigned line = (u8)(y - (sprite_y + 1));
        unsigned pattern_address = pattern_base +
            sprite_pattern_offset(mode, number, line);
        unsigned palette_base = (attr >> 2) & 0x30;
        u8 level = (attr & 0x20) ? 1 : 2;
        int sprite_x = pattern_mode_read(
            v9990, mode, address + 2, false) + 256 * (attr & 3);

        if (sprite_x > 1008)
            sprite_x -= 1024;
        for (unsigned x = 0; x < 16; x += 2) {
            u8 data = pattern_mode_read(
                v9990, mode, pattern_address++, true);
            unsigned colour[2] = {data >> 4, data & 15};

            for (unsigned half = 0; half < 2; ++half) {
                int output_x = sprite_x + (int)x + (int)half;

                if (!colour[half] || output_x < 0 ||
                    output_x >= (int)width)
                    continue;
                if (coverage[output_x] < level)
                    pixels[output_x] = palette_colour(
                        v9990, palette_base + colour[half]);
                coverage[output_x] = 2;
            }
        }
    }
}

typedef struct {
    bool visible;
    bool exclusive_or;
    unsigned x;
    u32 pattern;
    u32 colour;
} V9990CursorLine;

static V9990CursorLine bitmap_cursor_line(const V9990 *v9990,
                                          unsigned cursor, unsigned y) {
    V9990CursorLine result = {0};
    unsigned attribute = 0x7fe00 + cursor * 8;
    unsigned pattern_base = 0x7ff00 + cursor * 0x80;
    unsigned cursor_y = read_bx(v9990, attribute) +
        256u * (read_bx(v9990, attribute + 2) & 1u);
    unsigned line;
    u8 attr;

    cursor_y += (v9990->registers[V9990_REG_SCREEN_MODE_1] & 2) ? 2 : 1;
    line = (y - cursor_y) & 511;
    if (line >= 32)
        return result;
    attr = read_bx(v9990, attribute + 6);
    if ((attr & 0x10) || !(attr & 0xe0))
        return result;
    result.pattern =
        ((u32)read_bx(v9990, pattern_base + 4 * line) << 24) |
        ((u32)read_bx(v9990, pattern_base + 4 * line + 1) << 16) |
        ((u32)read_bx(v9990, pattern_base + 4 * line + 2) << 8) |
        read_bx(v9990, pattern_base + 4 * line + 3);
    if (!result.pattern)
        return result;
    result.visible = true;
    result.x = read_bx(v9990, attribute + 4) + 256u * (attr & 3);
    result.exclusive_or = (attr & 0xe0) == 0x20;
    result.colour = palette_colour(
        v9990,
        ((unsigned)v9990->registers[V9990_REG_SPRITE_PALETTE] << 2) +
        (attr >> 6));
    if (attr & 0x20)
        result.colour ^= 0x00ffffffu;
    return result;
}

static void render_bitmap_cursors(const V9990 *v9990,
                                  V9990DisplayMode mode,
                                  unsigned y, unsigned width,
                                  u32 *pixels) {
    V9990CursorLine cursors[2];
    unsigned y_offset = 0;

    if (v9990->registers[V9990_REG_CONTROL] & 0x40)
        return;
    if (mode == V9990_MODE_B0 || mode == V9990_MODE_B2 ||
        mode == V9990_MODE_B4)
        y_offset = (v9990->registers[V9990_REG_SCREEN_MODE_1] & 8)
                 ? 41 : 14;
    cursors[0] = bitmap_cursor_line(v9990, 0, y - y_offset);
    cursors[1] = bitmap_cursor_line(v9990, 1, y - y_offset);
    for (unsigned x = 0; x < width; ++x) {
        for (unsigned cursor = 0; cursor < 2; ++cursor) {
            V9990CursorLine *current = &cursors[cursor];
            unsigned offset;

            if (!current->visible || x < current->x)
                continue;
            offset = x - current->x;
            if (offset >= 32 ||
                !(current->pattern & (0x80000000u >> offset)))
                continue;
            pixels[x] = current->exclusive_or
                      ? pixels[x] ^ 0x00ffffffu
                      : current->colour;
            break;
        }
    }
}

void v9990_render(V9990 *v9990) {
    V9990DisplayMode mode;
    unsigned width;
    unsigned height;
    bool overscan;
    u8 coverage[V9990_MAX_WIDTH];

    if (!v9990 || !v9990->enabled || !v9990->pixels)
        return;
    mode = v9990_display_mode(v9990);
    overscan = mode == V9990_MODE_B0 || mode == V9990_MODE_B2 ||
               mode == V9990_MODE_B4;
    switch (mode) {
        case V9990_MODE_P1: width = 256; break;
        case V9990_MODE_P2: width = 512; break;
        case V9990_MODE_B0: width = 192; break;
        case V9990_MODE_B1: width = 256; break;
        case V9990_MODE_B2: width = 384; break;
        case V9990_MODE_B3: width = 512; break;
        case V9990_MODE_B4: width = 768; break;
        case V9990_MODE_B7: width = 1024; break;
        default: width = 256; break;
    }
    height = overscan
           ? ((v9990->registers[V9990_REG_SCREEN_MODE_1] & 8) ? 290 : 240)
           : 212;
    v9990->render_width = width;
    v9990->render_height = height;
    if (!(v9990->registers[V9990_REG_CONTROL] & 0x80)) {
        u32 backdrop = palette_colour(
            v9990, v9990->registers[V9990_REG_BACKDROP]);
        for (unsigned i = 0; i < width * height; ++i)
            v9990->pixels[i] = backdrop;
        return;
    }
    for (unsigned y = 0; y < height; ++y) {
        memset(coverage, 0, width);
        for (unsigned x = 0; x < width; ++x) {
            u32 colour;
            bool foreground = false;

            if (mode == V9990_MODE_P1)
                colour = render_p1_pixel(v9990, x, y, &foreground);
            else if (mode == V9990_MODE_P2)
                colour = render_p2_pixel(v9990, x, y, &foreground);
            else {
                unsigned sx = x + v9990->registers[V9990_REG_SCROLL_AX0] +
                    8u * v9990->registers[V9990_REG_SCROLL_AX1];
                unsigned sy = y + v9990->registers[V9990_REG_SCROLL_AY0] +
                    256u * v9990->registers[V9990_REG_SCROLL_AY1];
                colour = bitmap_colour(
                    v9990, sx, sy,
                    mode == V9990_MODE_B4 || mode == V9990_MODE_B7);
            }
            v9990->pixels[y * width + x] = colour;
            coverage[x] = foreground ? 1 : 0;
        }
        if (mode == V9990_MODE_P1 || mode == V9990_MODE_P2)
            render_pattern_sprites(
                v9990, mode, y, width,
                &v9990->pixels[y * width], coverage);
        else
            render_bitmap_cursors(
                v9990, mode, y, width, &v9990->pixels[y * width]);
    }
}

void v9990_init(V9990 *v9990) {
    if (v9990)
        memset(v9990, 0, sizeof(*v9990));
}

void v9990_destroy(V9990 *v9990) {
    if (!v9990)
        return;
    free(v9990->vram);
    free(v9990->pixels);
    memset(v9990, 0, sizeof(*v9990));
}

int v9990_set_enabled(V9990 *v9990, bool enabled) {
    if (!v9990)
        return -1;
    if (!enabled) {
        v9990->enabled = false;
        v9990->irq = false;
        return 0;
    }
    if (!v9990->vram)
        v9990->vram = malloc(V9990_VRAM_SIZE);
    if (!v9990->pixels)
        v9990->pixels = malloc(
            (size_t)V9990_MAX_WIDTH * V9990_MAX_HEIGHT *
            sizeof(*v9990->pixels));
    if (!v9990->vram || !v9990->pixels) {
        v9990_set_enabled(v9990, false);
        return -1;
    }
    v9990->enabled = true;
    for (unsigned address = 0; address < V9990_VRAM_SIZE; ++address)
        v9990->vram[address] = (address & 0x200) ? 0xff : 0x00;
    v9990_reset(v9990);
    return 0;
}

void v9990_reset(V9990 *v9990) {
    if (!v9990 || !v9990->enabled)
        return;
    memset(v9990->registers, 0, sizeof(v9990->registers));
    for (unsigned i = 0; i < 64; ++i) {
        v9990->palette[i * 4] = 0x9f;
        v9990->palette[i * 4 + 1] = 0x1f;
        v9990->palette[i * 4 + 2] = 0x1f;
        v9990->palette[i * 4 + 3] = 0;
    }
    v9990->register_select = 0xff;
    v9990->status = 0;
    v9990->pending_irqs = 0;
    v9990->command_status = 0;
    v9990->read_buffer = 0;
    v9990->read_address = 0;
    v9990->write_address = 0;
    v9990->system_reset = false;
    v9990->irq = false;
    v9990->render_width = 256;
    v9990->render_height = 212;
    v9990->frame_cycle = 0;
    v9990_render(v9990);
}

bool v9990_io_read(V9990 *v9990, u16 port, u8 *value) {
    unsigned low;

    if (!v9990 || !v9990->enabled || !value ||
        (u8)port < 0x60 || (u8)port > 0x6f)
        return false;
    low = (u8)port - 0x60;
    switch (low) {
        case V9990_PORT_VRAM:
            *value = v9990->read_buffer;
            if (!v9990->system_reset &&
                !(v9990->registers[5] & 0x80)) {
                v9990->read_address =
                    (v9990->read_address + 1) & (V9990_VRAM_SIZE - 1);
                set_register_address(v9990, 3, v9990->read_address);
                v9990->read_buffer = read_direct(
                    v9990, map_cpu_address(v9990, v9990->read_address));
            }
            return true;
        case V9990_PORT_PALETTE:
            *value = v9990->palette[
                v9990->registers[V9990_REG_PALETTE_POINTER]];
            return true;
        case V9990_PORT_COMMAND:
            *value = (v9990->command_status & V9990_STATUS_TR)
                   ? v9990->command_data : 0xff;
            if (v9990->command_cpu_read &&
                (v9990->command_status & V9990_STATUS_TR)) {
                if ((v9990->registers[V9990_REG_COMMAND] >> 4) == 3) {
                    consume_lmcm_data(v9990);
                } else {
                    v9990->command_status &= (u8)~V9990_STATUS_TR;
                    if (v9990->command_end_after_read)
                        finish_command(v9990);
                }
            }
            return true;
        case V9990_PORT_REGISTER_DATA:
            *value = read_register(
                v9990, v9990->register_select & 0x3f);
            if (!(v9990->register_select & 0x40))
                v9990->register_select =
                    (u8)((v9990->register_select + 1) & ~0x40);
            return true;
        case V9990_PORT_STATUS: {
            unsigned phase = v9990->frame_cycles
                           ? (unsigned)((u64)v9990->frame_cycle *
                             v9990->frame_scanlines * 2736u /
                             v9990->frame_cycles) : 0;
            unsigned line = phase / 2736u;
            unsigned x = phase % 2736u;
            bool vertical = line < 15 ||
                line >= ((v9990->registers[7] & 8) ? 305u : 247u);
            bool horizontal = x < 400 || x >= 2560;
            *value = (u8)(v9990->command_status |
                (vertical ? 0x40 : 0) | (horizontal ? 0x20 : 0) |
                (v9990->status & 6));
            return true;
        }
        case V9990_PORT_INTERRUPT:
            *value = v9990->pending_irqs;
            return true;
        default:
            *value = 0xff;
            return true;
    }
}

bool v9990_io_write(V9990 *v9990, u16 port, u8 value) {
    unsigned low;

    if (!v9990 || !v9990->enabled ||
        (u8)port < 0x60 || (u8)port > 0x6f)
        return false;
    low = (u8)port - 0x60;
    switch (low) {
        case V9990_PORT_VRAM:
            if (!v9990->system_reset) {
                write_direct(
                    v9990,
                    map_cpu_address(v9990, v9990->write_address), value);
                if (!(v9990->registers[2] & 0x80)) {
                    v9990->write_address =
                        (v9990->write_address + 1) &
                        (V9990_VRAM_SIZE - 1);
                    set_register_address(v9990, 0,
                                         v9990->write_address);
                }
            }
            return true;
        case V9990_PORT_PALETTE: {
            unsigned pointer =
                v9990->registers[V9990_REG_PALETTE_POINTER];
            if (v9990->system_reset)
                pointer = value = 0;
            switch (pointer & 3) {
                case 0: value &= 0x9f; break;
                case 1:
                case 2: value &= 0x1f; break;
                default: value = 0; break;
            }
            v9990->palette[pointer] = value;
            if (!(v9990->registers[V9990_REG_PALETTE_CONTROL] & 0x10)) {
                if ((pointer & 3) < 2)
                    ++pointer;
                else if ((pointer & 3) == 2)
                    pointer += 2;
                else
                    pointer -= 3;
                v9990->registers[V9990_REG_PALETTE_POINTER] =
                    (u8)pointer;
            }
            return true;
        }
        case V9990_PORT_COMMAND:
            v9990->command_data = value;
            if (v9990->command_cpu_write &&
                (v9990->command_status & V9990_STATUS_TR)) {
                unsigned opcode =
                    v9990->registers[V9990_REG_COMMAND] >> 4;
                v9990->command_status &= (u8)~V9990_STATUS_TR;
                if (opcode == 5) { /* CMMC: one source bit per pixel. */
                    u16 foreground =
                        (u16)(v9990->registers[48] |
                              ((u16)v9990->registers[49] << 8));
                    u16 background =
                        (u16)(v9990->registers[50] |
                              ((u16)v9990->registers[51] << 8));

                    for (unsigned bit = 0;
                         bit < 8 && v9990->command_cpu_write; ++bit) {
                        pset(v9990, v9990->command_dx,
                             v9990->command_dy,
                             (value & (0x80u >> bit))
                                 ? foreground : background);
                        advance_command_position(v9990, false);
                    }
                } else { /* LMMC: packed pixels in the active colour mode. */
                    unsigned bpp = bits_per_pixel(v9990);
                    unsigned pixels = bpp < 8 ? 8 / bpp : 1;

                    for (unsigned i = 0;
                         i < pixels && v9990->command_cpu_write; ++i) {
                        unsigned shift =
                            bpp < 8 ? 8 - bpp * (i + 1) : 0;
                        u16 colour = bpp == 16 ? value :
                            (u16)((value >> shift) &
                                  ((1u << bpp) - 1));
                        pset(v9990, v9990->command_dx,
                             v9990->command_dy, colour);
                        advance_command_position(v9990, false);
                    }
                }
                if (v9990->command_cpu_write)
                    v9990->command_status |= V9990_STATUS_TR;
            }
            return true;
        case V9990_PORT_REGISTER_DATA:
            write_register(v9990, v9990->register_select & 0x3f,
                           v9990->system_reset ? 0 : value);
            if (!(v9990->register_select & 0x80))
                v9990->register_select =
                    (u8)((v9990->register_select & 0xc0) |
                         ((v9990->register_select + 1) & 0x3f));
            return true;
        case V9990_PORT_REGISTER_SELECT:
            v9990->register_select = v9990->system_reset ? 0 : value;
            return true;
        case V9990_PORT_INTERRUPT:
            v9990->pending_irqs &= (u8)~value;
            update_irq(v9990);
            return true;
        case V9990_PORT_SYSTEM: {
            bool new_reset = (value & 2) != 0;
            v9990->status =
                (u8)((v9990->status & 0xfb) | ((value & 1) << 2));
            if (new_reset && !v9990->system_reset) {
                memset(v9990->registers, 0,
                       sizeof(v9990->registers));
                v9990->pending_irqs = 0;
                v9990->command_status = 0;
                update_irq(v9990);
            }
            v9990->system_reset = new_reset;
            return true;
        }
        default:
            return true;
    }
}

void v9990_begin_frame(V9990 *v9990, unsigned frame_cycles) {
    if (!v9990 || !v9990->enabled)
        return;
    v9990->frame_cycles = frame_cycles;
    v9990->frame_cycle = 0;
    v9990->frame_scanlines =
        (v9990->registers[V9990_REG_SCREEN_MODE_1] & 8) ? 313u : 262u;
    v9990->status ^= 0x02;
}

void v9990_advance(V9990 *v9990, unsigned cycles) {
    unsigned previous;
    unsigned threshold;

    if (!v9990 || !v9990->enabled || !v9990->frame_cycles)
        return;
    previous = v9990->frame_cycle;
    v9990->frame_cycle += cycles;
    if (v9990->frame_cycle > v9990->frame_cycles)
        v9990->frame_cycle = v9990->frame_cycles;
    threshold = v9990->frame_cycles * 9 / 10;
    if (previous < threshold && v9990->frame_cycle >= threshold) {
        v9990->pending_irqs |= V9990_IRQ_VERTICAL;
        update_irq(v9990);
    }
}

void v9990_end_frame(V9990 *v9990) {
    if (!v9990 || !v9990->enabled)
        return;
    v9990_render(v9990);
}

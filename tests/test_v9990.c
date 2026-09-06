#include "v9990.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void write_register(V9990 *v9990, u8 reg, u8 value) {
    assert(v9990_io_write(v9990, 0x64, (u8)(0x80 | reg)));
    assert(v9990_io_write(v9990, 0x63, value));
}

static u8 read_register(V9990 *v9990, u8 reg) {
    u8 value = 0;

    assert(v9990_io_write(v9990, 0x64, (u8)(0x40 | reg)));
    assert(v9990_io_read(v9990, 0x63, &value));
    return value;
}

static void write_vram(V9990 *v9990, unsigned address,
                       const u8 *data, size_t size) {
    write_register(v9990, 0, (u8)address);
    write_register(v9990, 1, (u8)(address >> 8));
    write_register(v9990, 2, (u8)(address >> 16));
    for (size_t i = 0; i < size; ++i)
        assert(v9990_io_write(v9990, 0x60, data[i]));
}

static void test_enable_and_ports(void) {
    V9990 v9990;
    u8 value = 0;

    v9990_init(&v9990);
    assert(!v9990_io_read(&v9990, 0x60, &value));
    assert(v9990_set_enabled(&v9990, true) == 0);
    assert(v9990.enabled);
    assert(v9990.vram != NULL);
    assert(v9990.pixels != NULL);
    assert(v9990.render_width == 256);
    assert(v9990.render_height == 212);

    write_register(&v9990, 6, 0x81);
    assert(v9990_display_mode(&v9990) == V9990_MODE_B1);
    assert(strcmp(v9990_mode_name(&v9990), "B1") == 0);
    assert(read_register(&v9990, 6) == 0x81);

    assert(v9990_set_enabled(&v9990, false) == 0);
    assert(!v9990.enabled);
    assert(!v9990_io_read(&v9990, 0x60, &value));
    v9990_destroy(&v9990);
}

static void test_vram_and_palette_ports(void) {
    V9990 v9990;
    u8 value = 0;

    v9990_init(&v9990);
    assert(v9990_set_enabled(&v9990, true) == 0);

    /* P1 uses linear CPU addressing. Program write address 0x01234. */
    write_register(&v9990, 0, 0x34);
    write_register(&v9990, 1, 0x12);
    write_register(&v9990, 2, 0x00);
    assert(v9990_io_write(&v9990, 0x60, 0x83));
    assert(v9990.vram[0x1234] == 0x83);
    assert(v9990.write_address == 0x1235);

    /* The read port is buffered, matching the V9990's CPU interface. */
    write_register(&v9990, 3, 0x34);
    write_register(&v9990, 4, 0x12);
    write_register(&v9990, 5, 0x00);
    assert(v9990_io_read(&v9990, 0x60, &value));
    assert(value == 0x83);

    write_register(&v9990, 14, 0);
    assert(v9990_io_write(&v9990, 0x61, 0x1f));
    assert(v9990_io_write(&v9990, 0x61, 0x12));
    assert(v9990_io_write(&v9990, 0x61, 0x0a));
    assert(v9990.palette[0] == 0x1f);
    assert(v9990.palette[1] == 0x12);
    assert(v9990.palette[2] == 0x0a);
    assert(v9990.registers[14] == 4);

    v9990_destroy(&v9990);
}

static void test_bitmap_render_and_interrupt(void) {
    V9990 v9990;
    u8 status = 0;

    v9990_init(&v9990);
    assert(v9990_set_enabled(&v9990, true) == 0);
    write_register(&v9990, 6, 0x81); /* B1, 4 bits per pixel. */
    write_register(&v9990, 8, 0x80); /* Display enable. */
    write_register(&v9990, 14, 4);   /* Palette entry 1. */
    assert(v9990_io_write(&v9990, 0x61, 0x1f));
    assert(v9990_io_write(&v9990, 0x61, 0x00));
    assert(v9990_io_write(&v9990, 0x61, 0x00));
    assert(v9990_io_write(&v9990, 0x61, 0x00));
    assert(v9990_io_write(&v9990, 0x61, 0x1f));
    assert(v9990_io_write(&v9990, 0x61, 0x00));

    /* Bitmap logical byte zero maps to physical byte zero in B modes. */
    write_register(&v9990, 0, 0);
    write_register(&v9990, 1, 0);
    write_register(&v9990, 2, 0);
    assert(v9990_io_write(&v9990, 0x60, 0x12));
    v9990_render(&v9990);
    assert(v9990.render_width == 256);
    assert(v9990.render_height == 212);
    assert(v9990.pixels[0] == 0xff0000);
    assert(v9990.pixels[1] == 0x00ff00);

    write_register(&v9990, 9, 1);
    v9990_begin_frame(&v9990, 1000);
    v9990_advance(&v9990, 900);
    assert(v9990.irq);
    assert(v9990_io_read(&v9990, 0x66, &status));
    assert(status & 1);
    assert(v9990_io_write(&v9990, 0x66, 1));
    assert(!v9990.irq);

    v9990_destroy(&v9990);
}

static void test_bitmap_hardware_cursors(void) {
    static const u8 attributes[] = {
        0x00, 0x00, 0x00, 0x00, /* Y and reserved fields. */
        0x05, 0x00, 0x40        /* X=5, palette colour 1. */
    };
    static const u8 pattern[] = {0x80, 0x00, 0x00, 0x00};
    static const u8 background[] = {0x01};
    V9990 v9990;

    v9990_init(&v9990);
    assert(v9990_set_enabled(&v9990, true) == 0);
    memset(v9990.palette, 0, sizeof(v9990.palette));
    v9990.palette[6] = 0x1f;  /* Bitmap palette 1: blue. */
    v9990.palette[20] = 0x1f; /* Cursor palette 5: white. */
    v9990.palette[21] = 0x1f;
    v9990.palette[22] = 0x1f;
    write_register(&v9990, 6, 0x81); /* B1, 4 bits per pixel. */
    write_register(&v9990, 8, 0x80); /* Display and cursors enabled. */
    write_register(&v9990, 28, 0x01); /* Cursor palette starts at 4. */
    write_vram(&v9990, 128 + 2, background, sizeof(background));
    write_vram(&v9990, 0x7fe00, attributes, sizeof(attributes));
    write_vram(&v9990, 0x7ff00, pattern, sizeof(pattern));

    v9990_render(&v9990);
    assert(v9990.pixels[256 + 5] == 0xffffff);

    /* CC=1 with EOR set complements its selected palette colour. */
    {
        const u8 palette_and_eor = 0x60;
        v9990.palette[20] = 0;
        v9990.palette[21] = 0;
        v9990.palette[22] = 0;
        write_vram(&v9990, 0x7fe06, &palette_and_eor, 1);
    }
    v9990_render(&v9990);
    assert(v9990.pixels[256 + 5] == 0xffffff);

    /* The EOR cursor mode complements the underlying bitmap pixel. */
    {
        const u8 exclusive_or = 0x20;
        write_vram(&v9990, 0x7fe06, &exclusive_or, 1);
    }
    v9990_render(&v9990);
    assert(v9990.pixels[256 + 5] == 0xffff00);

    /* R#8 bit 6 suppresses both bitmap cursors. */
    write_register(&v9990, 8, 0xc0);
    v9990_render(&v9990);
    assert(v9990.pixels[256 + 5] == 0x0000ff);
    v9990_destroy(&v9990);
}

static void test_bitmap_cursor_overscan_offset(void) {
    static const u8 attributes[] = {
        0x00, 0x00, 0x00, 0x00,
        0x05, 0x00, 0x40
    };
    static const u8 pattern[] = {0x80, 0x00, 0x00, 0x00};
    static const u8 zero = 0;
    V9990 v9990;

    v9990_init(&v9990);
    assert(v9990_set_enabled(&v9990, true) == 0);
    memset(v9990.palette, 0, sizeof(v9990.palette));
    v9990.palette[4] = 0x1f;
    v9990.palette[5] = 0x1f;
    v9990.palette[6] = 0x1f;
    assert(v9990_io_write(&v9990, 0x67, 0x01)); /* XTAL clock. */
    write_register(&v9990, 6, 0x91); /* B2 overscan, 4 bits per pixel. */
    write_register(&v9990, 8, 0x80);
    write_vram(&v9990, 128 + 2, &zero, 1);
    write_vram(&v9990, 15 * 128 + 2, &zero, 1);
    write_vram(&v9990, 42 * 128 + 2, &zero, 1);
    write_vram(&v9990, 0x7fe00, attributes, sizeof(attributes));
    write_vram(&v9990, 0x7ff00, pattern, sizeof(pattern));

    v9990_render(&v9990);
    assert(v9990.render_width == 384);
    assert(v9990.render_height == 240);
    assert(v9990.pixels[1 * 384 + 5] == 0x000000);
    assert(v9990.pixels[15 * 384 + 5] == 0xffffff);

    write_register(&v9990, 7, 0x08); /* PAL overscan uses a 41-line offset. */
    v9990_render(&v9990);
    assert(v9990.render_height == 290);
    assert(v9990.pixels[15 * 384 + 5] == 0x000000);
    assert(v9990.pixels[42 * 384 + 5] == 0xffffff);
    v9990_destroy(&v9990);
}

static void test_pattern_mode_sprites(void) {
    static const u8 attributes[] = {0x00, 0x00, 0x03, 0x00};
    static const u8 pattern[] = {0x10};
    static const u8 zero[] = {0x00, 0x00};
    V9990 v9990;

    v9990_init(&v9990);
    assert(v9990_set_enabled(&v9990, true) == 0);
    memset(v9990.palette, 0, sizeof(v9990.palette));
    v9990.palette[4] = 0x1f;
    v9990.palette[5] = 0x1f;
    v9990.palette[6] = 0x1f;
    write_register(&v9990, 6, 0x00); /* P1. */
    write_register(&v9990, 8, 0x80);
    write_register(&v9990, 25, 0x02); /* Pattern base 0x8000. */
    write_vram(&v9990, 0x7c000, zero, sizeof(zero));
    write_vram(&v9990, 0x7e000, zero, sizeof(zero));
    write_vram(&v9990, 129, zero, 1);
    write_vram(&v9990, 0x40000 + 129, zero, 1);
    write_vram(&v9990, 0x3fe00, attributes, sizeof(attributes));
    write_vram(&v9990, 0x08000, pattern, sizeof(pattern));

    v9990_render(&v9990);
    assert(v9990.pixels[256 + 3] == 0xffffff);
    write_register(&v9990, 8, 0xc0);
    v9990_render(&v9990);
    assert(v9990.pixels[256 + 3] != 0xffffff);
    v9990_destroy(&v9990);
}

static void test_command_opcode_uses_high_nibble(void) {
    V9990 v9990;

    v9990_init(&v9990);
    assert(v9990_set_enabled(&v9990, true) == 0);
    write_register(&v9990, 6, 0x81); /* B1, 4 bits per pixel. */
    write_register(&v9990, 36, 0);   /* DX */
    write_register(&v9990, 37, 0);
    write_register(&v9990, 38, 0);   /* DY */
    write_register(&v9990, 39, 0);
    write_register(&v9990, 40, 2);   /* NX */
    write_register(&v9990, 41, 0);
    write_register(&v9990, 42, 1);   /* NY */
    write_register(&v9990, 43, 0);
    write_register(&v9990, 45, 0x0c); /* source copy */
    write_register(&v9990, 46, 0xff);
    write_register(&v9990, 47, 0xff);
    write_register(&v9990, 48, 0x03); /* foreground colour */
    write_register(&v9990, 49, 0x00);
    write_register(&v9990, 52, 0x20); /* LMMV opcode 2 lives in bits 7-4. */
    assert(v9990.vram[0] == 0x33);
    assert((v9990.command_status & 1) == 0);

    write_register(&v9990, 36, 0);
    write_register(&v9990, 37, 0);
    write_register(&v9990, 38, 1);
    write_register(&v9990, 39, 0);
    write_register(&v9990, 40, 8);
    write_register(&v9990, 41, 0);
    write_register(&v9990, 42, 1);
    write_register(&v9990, 43, 0);
    write_register(&v9990, 50, 0);
    write_register(&v9990, 51, 0);
    write_register(&v9990, 52, 0x50); /* CMMC expands eight source bits. */
    assert(v9990_io_write(&v9990, 0x62, 0x80));
    assert(v9990.vram[64] == 0x30);
    assert(v9990.vram[0x40040] == 0x00);
    assert(v9990.vram[65] == 0x00);
    assert(v9990.vram[0x40041] == 0x00);
    assert((v9990.command_status & 1) == 0);
    v9990_destroy(&v9990);
}

static void test_lmcm_cpu_read_transfer(void) {
    static const u8 source[] = {0x12, 0x30};
    V9990 v9990;
    u8 value = 0;

    v9990_init(&v9990);
    assert(v9990_set_enabled(&v9990, true) == 0);
    write_register(&v9990, 6, 0x81); /* B1, 4 bits per pixel. */
    write_vram(&v9990, 0, source, sizeof(source));
    write_register(&v9990, 32, 0);   /* SX */
    write_register(&v9990, 33, 0);
    write_register(&v9990, 34, 0);   /* SY */
    write_register(&v9990, 35, 0);
    write_register(&v9990, 40, 3);   /* Three pixels -> two CPU bytes. */
    write_register(&v9990, 41, 0);
    write_register(&v9990, 42, 1);
    write_register(&v9990, 43, 0);
    write_register(&v9990, 52, 0x30); /* LMCM. */

    assert((v9990.command_status & 0x81) == 0x81);
    assert(v9990_io_read(&v9990, 0x62, &value));
    assert(value == 0x12);
    assert((v9990.command_status & 0x81) == 0x81);
    assert(v9990.command_remaining_x == 3);
    assert(v9990.command_remaining_y == 0);
    assert(v9990_io_read(&v9990, 0x62, &value));
    assert(value == 0x30);
    assert((v9990.command_status & 0x81) == 0);
    assert(v9990_io_read(&v9990, 0x62, &value));
    assert(value == 0xff);
    v9990_destroy(&v9990);

    v9990_init(&v9990);
    assert(v9990_set_enabled(&v9990, true) == 0);
    write_register(&v9990, 6, 0x83); /* B1, 16 bits per pixel. */
    v9990.vram[0] = 0x34;
    v9990.vram[0x40000] = 0x12;
    write_register(&v9990, 32, 0);
    write_register(&v9990, 33, 0);
    write_register(&v9990, 34, 0);
    write_register(&v9990, 35, 0);
    write_register(&v9990, 40, 1);
    write_register(&v9990, 41, 0);
    write_register(&v9990, 42, 1);
    write_register(&v9990, 43, 0);
    write_register(&v9990, 52, 0x30);
    assert(v9990_io_read(&v9990, 0x62, &value));
    assert(value == 0x34);
    assert((v9990.command_status & 0x81) == 0x81);
    assert(v9990_io_read(&v9990, 0x62, &value));
    assert(value == 0x12);
    assert((v9990.command_status & 0x81) == 0);
    v9990_destroy(&v9990);
}

int main(void) {
    test_enable_and_ports();
    test_vram_and_palette_ports();
    test_bitmap_render_and_interrupt();
    test_bitmap_hardware_cursors();
    test_bitmap_cursor_overscan_offset();
    test_pattern_mode_sprites();
    test_command_opcode_uses_high_nibble();
    test_lmcm_cpu_read_transfer();
    puts("V9990 port, VRAM, palette, sprite/cursor, renderer, and IRQ tests passed");
    return 0;
}

#include "asic.h"

#include <assert.h>
#include <string.h>

static void init_parts(Asic *asic, GateArray *ga, Mem *mem, PSG *psg) {
    mem_init(mem);
    ga_init(ga);
    psg_init(psg);
    asic_reset(asic, ga);
}

static void test_palette_and_sprite_registers(void) {
    Asic asic;
    GateArray ga = {0};
    Mem mem;
    PSG psg;
    init_parts(&asic, &ga, &mem, &psg);

    asic_register_write(&asic, &ga, &mem, 0x6402, 0xF3);
    asic_register_write(&asic, &ga, &mem, 0x6403, 0x07);
    assert(asic.palette[1] == 0xFF7733);
    assert(ga.resolved_ink[1] == 0xFF7733);

    asic_register_write(&asic, &ga, &mem, 0x4123, 0x1E);
    assert(asic.sprite[1][3][2] == 0x0E);
    asic_register_write(&asic, &ga, &mem, 0x6000, 0x34);
    asic_register_write(&asic, &ga, &mem, 0x6001, 0x02);
    asic_register_write(&asic, &ga, &mem, 0x6002, 0x56);
    asic_register_write(&asic, &ga, &mem, 0x6003, 0x01);
    asic_register_write(&asic, &ga, &mem, 0x6004, 0x0F);
    assert(asic.sprite_x[0] == 0x234);
    assert(asic.sprite_y[0] == 0x156);
    assert(asic.sprite_mag_x[0] == 4);
    assert(asic.sprite_mag_y[0] == 4);

    /* Offsets 4-7 all alias the write-only magnification register. */
    asic_register_write(&asic, &ga, &mem, 0x6007, 0x06);
    assert(asic.sprite_mag_x[0] == 1);
    assert(asic.sprite_mag_y[0] == 2);
}

static void test_raster_split_and_dma(void) {
    Asic asic;
    GateArray ga = {0};
    Mem mem;
    PSG psg;
    CRTC crtc;
    init_parts(&asic, &ga, &mem, &psg);
    crtc_init(&crtc);

    asic_register_write(&asic, &ga, &mem, 0x6800, 3);
    crtc.vcc = 0;
    crtc.vlc = 3;
    crtc.hcc = crtc.reg[2] + 9;
    asic_raster_tick(&asic, &crtc, &mem, &ga);
    assert(!ga.interrupt_pending);
    crtc.hcc++;
    asic_raster_tick(&asic, &crtc, &mem, &ga);
    assert(ga.interrupt_pending);
    assert(mem.plus_registers[0x2C0F] == 0x00);
    asic_irq_ack(&asic, &mem, &ga);
    assert(!asic.raster_interrupt);
    assert(mem.plus_registers[0x2C0F] == 0x80);

    /* The live VCC is six bits wide. Its rows 32..63 must not wrap into the
     * five-bit PRI comparator and generate a second interrupt. */
    ga.interrupt_pending = false;
    crtc.vcc = 32;
    crtc.vlc = 3;
    asic_raster_tick(&asic, &crtc, &mem, &ga);
    assert(!ga.interrupt_pending);

    asic_register_write(&asic, &ga, &mem, 0x6801, 3);
    asic_register_write(&asic, &ga, &mem, 0x6802, 0x12);
    asic_register_write(&asic, &ga, &mem, 0x6803, 0x34);
    crtc.ma = 0x3010;
    crtc.ma_row_start = 0x3000;
    crtc.ma_next_row = 0x3040;
    crtc.reg[1] = 40;
    crtc.hcc = 40;
    crtc.vcc = 0;
    crtc.vlc = 3;
    asic_latch_split(&asic, &crtc, 0, 3, false);
    asic_register_write(&asic, &ga, &mem, 0x6802, 0x3A);
    asic_register_write(&asic, &ga, &mem, 0x6803, 0xBC);
    crtc.ma = 0x3040;
    asic_apply_split(&asic, &crtc);
    assert(crtc.ma == 0x3040);
    assert(crtc.ma_row_start == 0x3000);
    assert(crtc.ma_next_row == 0x3040);
    assert(asic_video_ma(&asic, 0x3040) == 0x1234);
    assert(asic_video_ma(&asic, 0x3045) == 0x1239);

    /* A later split is based on the unmodified CRTC address, not on the
     * previously translated video address. */
    asic_register_write(&asic, &ga, &mem, 0x6801, 4);
    asic_register_write(&asic, &ga, &mem, 0x6802, 0x23);
    asic_register_write(&asic, &ga, &mem, 0x6803, 0x40);
    crtc.vlc = 4;
    asic_latch_split(&asic, &crtc, 0, 4, false);
    crtc.ma = 0x3090;
    asic_apply_split(&asic, &crtc);
    assert(asic_video_ma(&asic, 0x3090) == 0x2340);
    assert(asic_video_ma(&asic, 0x3093) == 0x2343);
    asic_new_frame(&asic);
    assert(asic_video_ma(&asic, 0x3093) == 0x3093);

    /* A program may set SSSL after R1 while the selected line is already
     * active. The Plus still applies that split at the end of the line. */
    Asic late_asic = {0};
    CRTC late_crtc;
    crtc_init(&late_crtc);
    late_crtc.reg[1] = 34;
    late_crtc.reg[4] = 38;
    late_crtc.reg[9] = 7;
    late_crtc.hcc = 59;
    late_crtc.vcc = 12;
    late_crtc.vlc = 0;
    asic_register_write(&late_asic, &ga, &mem, 0x6801, 0x60);
    asic_register_write(&late_asic, &ga, &mem, 0x6802, 0x11);
    asic_register_write(&late_asic, &ga, &mem, 0x6803, 0x79);
    asic_latch_split(&late_asic, &late_crtc, 12, 0, false);
    assert(!late_asic.split_pending);
    late_crtc.hcc = 0;
    late_crtc.vlc = 1;
    asic_latch_split(&late_asic, &late_crtc, 12, 0, true);
    assert(late_asic.split_pending);
    assert(late_asic.split_pending_base == 0x1179);

    /* Likewise, VCC 34/RA 2 must not alias the split programmed for line
     * 0x12 (VCC 2/RA 2). */
    Asic compare_asic = {0};
    CRTC compare_crtc;
    crtc_init(&compare_crtc);
    compare_crtc.reg[1] = 34;
    compare_crtc.hcc = 34;
    compare_crtc.vcc = 34;
    compare_crtc.vlc = 2;
    asic_register_write(&compare_asic, &ga, &mem, 0x6801, 0x12);
    asic_register_write(&compare_asic, &ga, &mem, 0x6802, 0x20);
    asic_register_write(&compare_asic, &ga, &mem, 0x6803, 0x00);
    asic_latch_split(&compare_asic, &compare_crtc, 34, 2, false);
    assert(!compare_asic.split_pending);
    compare_crtc.vcc = 2;
    asic_latch_split(&compare_asic, &compare_crtc, 2, 2, false);
    assert(compare_asic.split_pending);

    /* A split address overrides the fine-scroll row advance. Its first
     * displayed scanline must begin at SSA even when RA+SSCR wraps R9. */
    Asic wrap_asic = {
        .split_pending_base = 0x1234,
        .split_pending = true,
        .vscroll = 2,
    };
    CRTC wrap_crtc;
    crtc_init(&wrap_crtc);
    wrap_crtc.reg[1] = 49;
    wrap_crtc.reg[9] = 7;
    wrap_crtc.ma = 0x3040;
    wrap_crtc.vlc = 6;
    asic_apply_split(&wrap_asic, &wrap_crtc);
    AsicVideoPosition split_pos = asic_video_position(
        &wrap_asic, wrap_crtc.ma, wrap_crtc.vlc,
        wrap_crtc.reg[9], wrap_crtc.reg[1]);
    assert(split_pos.ma == 0x1234);
    assert(split_pos.raster == 0);
    split_pos = asic_video_position(&wrap_asic, wrap_crtc.ma, 7, 7, 49);
    assert(split_pos.ma == 0x1234);
    assert(split_pos.raster == 1);
    split_pos = asic_video_position(
        &wrap_asic, wrap_crtc.ma + 49, 0, 7, 49);
    assert(split_pos.ma == 0x1234);
    assert(split_pos.raster == 2);

    /* Fine vertical scroll wraps into a row whose width comes from R1.
     * Sonic GX uses 49 characters, so a firmware-width constant corrupts
     * the wrapped scanlines into displaced horizontal strips. */
    asic.vscroll = 2;
    AsicVideoPosition pos = asic_video_position(&asic, 0x3005, 4, 7, 49);
    assert(pos.ma == 0x3005);
    assert(pos.raster == 6);
    pos = asic_video_position(&asic, 0x3005, 7, 7, 49);
    assert(pos.ma == 0x3036);
    assert(pos.raster == 1);
    asic.vscroll = 6;
    pos = asic_video_position(&asic, 0x3005, 3, 3, 49);
    assert(pos.ma == 0x3067);
    assert(pos.raster == 1);

    /* A programmable raster replaces legacy GA IRQ requests, but the GA
     * counter itself continues running and wrapping. */
    ga.interrupt_pending = false;
    for (int i = 0; i < 52; i++)
        asic_hsync(&asic, &mem, &psg, &ga);
    assert(ga.interrupt_counter == 0);
    assert(!ga.interrupt_pending);
    asic_new_frame(&asic);
    assert(asic.scanline == 0);

    /* DMA LOAD R8,0x0F from 0x1000. */
    mem.ram[0x1000] = 0x0F;
    mem.ram[0x1001] = 0x08;
    asic_register_write(&asic, &ga, &mem, 0x6C00, 0x00);
    asic_register_write(&asic, &ga, &mem, 0x6C01, 0x10);
    asic_register_write(&asic, &ga, &mem, 0x6C0F, 0x01);
    asic_hsync(&asic, &mem, &psg, &ga);
    assert(psg.reg[8] == 0x0F);
    assert(asic.dma[0].source == 0x1002);
}

static void test_vectored_interrupts(void) {
    Asic asic;
    GateArray ga = {0};
    Mem mem;
    PSG psg;
    init_parts(&asic, &ga, &mem, &psg);

    /* IVR base A0, automatic DMA acknowledge enabled. */
    asic_register_write(&asic, &ga, &mem, 0x6805, 0xA0);
    asic.dma[0].interrupt = true;
    asic.dma[1].interrupt = true;
    asic.dma[2].interrupt = true;
    assert(asic_irq_pending(&asic));
    assert(asic_irq_vector(&asic) == 0xA0); /* DMA 2 has priority. */

    asic_irq_ack(&asic, &mem, &ga);
    assert(!asic.dma[2].interrupt);
    assert(asic_irq_vector(&asic) == 0xA2);
    assert(mem.plus_registers[0x2C0F] == 0x60);

    asic.raster_interrupt = true;
    ga.interrupt_counter = 40;
    assert(asic_irq_vector(&asic) == 0xA6);
    asic_irq_ack(&asic, &mem, &ga);
    assert(!asic.raster_interrupt);
    assert(ga.interrupt_counter == 8);
    assert(mem.plus_registers[0x2C0F] == 0xE0);

    /* IVR bit 0 leaves a DMA request asserted until DCSR acknowledges it. */
    asic_register_write(&asic, &ga, &mem, 0x6805, 0xA1);
    asic_irq_ack(&asic, &mem, &ga);
    assert(asic.dma[1].interrupt);
    assert(ga.interrupt_pending);
    assert((mem.plus_registers[0x2C0F] & 0x80) == 0);
    asic_register_write(&asic, &ga, &mem, 0x6C0F, 0x20);
    assert(!asic.dma[1].interrupt);
}

static void test_dma_pause_cadence(void) {
    Asic asic;
    GateArray ga = {0};
    Mem mem;
    PSG psg;
    init_parts(&asic, &ga, &mem, &psg);

    /* Sonic GX drives split-screen handlers with PAUSE 7, INT pairs. The
     * interrupts must remain eight scanlines apart, not drift to nine. */
    mem.ram[0x2000] = 0x07; mem.ram[0x2001] = 0x10;
    mem.ram[0x2002] = 0x10; mem.ram[0x2003] = 0x40;
    mem.ram[0x2004] = 0x07; mem.ram[0x2005] = 0x10;
    mem.ram[0x2006] = 0x10; mem.ram[0x2007] = 0x40;
    asic.dma[2].source = 0x2000;
    asic.dma[2].enabled = true;
    asic.interrupt_vector = 0; /* automatic DMA acknowledgement */

    asic_hsync(&asic, &mem, &psg, &ga); /* PAUSE 7 */
    for (int i = 0; i < 6; i++) {
        asic_hsync(&asic, &mem, &psg, &ga);
        assert(!asic.dma[2].interrupt);
    }
    asic_hsync(&asic, &mem, &psg, &ga); /* INT */
    assert(asic.dma[2].interrupt);
    asic_irq_ack(&asic, &mem, &ga);

    for (int i = 0; i < 7; i++) {
        asic_hsync(&asic, &mem, &psg, &ga);
        assert(!asic.dma[2].interrupt);
    }
    asic_hsync(&asic, &mem, &psg, &ga); /* next INT, eight lines later */
    assert(asic.dma[2].interrupt);

    /* PAUSE 2 with prescaler 1 delays the following instruction by four
     * HSYNC periods. */
    asic_reset(&asic, &ga);
    memset(mem.ram + 0x2100, 0, 8);
    mem.ram[0x2100] = 0x02; mem.ram[0x2101] = 0x10;
    mem.ram[0x2102] = 0x10; mem.ram[0x2103] = 0x40;
    asic.dma[0].source = 0x2100;
    asic.dma[0].prescaler = 1;
    asic.dma[0].enabled = true;
    asic_hsync(&asic, &mem, &psg, &ga);
    for (int i = 0; i < 3; i++) {
        asic_hsync(&asic, &mem, &psg, &ga);
        assert(!asic.dma[0].interrupt);
    }
    asic_hsync(&asic, &mem, &psg, &ga);
    assert(asic.dma[0].interrupt);
}

static void test_sprite_beam_composition(void) {
    Asic asic = {0};
    u32 pixels[16];

    for (int i = 0; i < 16; i++)
        pixels[i] = 0x010203;
    asic.palette[17] = 0x112233;
    asic.palette[18] = 0x445566;
    asic.sprite_mag_x[0] = 1;
    asic.sprite_mag_y[0] = 1;
    asic.sprite_x[0] = 80;
    asic.sprite_y[0] = 48;
    asic.sprite[0][0][0] = 1;
    asic.sprite[0][1][0] = 2;

    asic_draw_sprites_char(&asic, 5, 6, 0, 40, pixels);
    assert(pixels[0] == 0x112233);
    assert(pixels[1] == 0x445566);
    assert(pixels[2] == 0x010203);

    /* Lower sprite numbers have priority; pen zero remains transparent. */
    asic.sprite_mag_x[1] = 1;
    asic.sprite_mag_y[1] = 1;
    asic.sprite[1][0][0] = 2;
    pixels[0] = pixels[1] = 0;
    asic.sprite_x[1] = 80;
    asic.sprite_y[1] = 48;
    asic_draw_sprites_char(&asic, 5, 6, 0, 40, pixels);
    assert(pixels[0] == 0x112233);
    assert(pixels[1] == 0x445566);

    /* A wrapped negative X (0x3FF = -1) renders at the display left edge. */
    asic.sprite_x[0] = 0x3FF;
    asic.sprite[0][1][0] = 1;
    pixels[0] = pixels[1] = 0;
    asic_draw_sprites_char(&asic, 0, 6, 0, 40, pixels);
    assert(pixels[0] == 0x112233);

    /* Each CRTC character advances 16 sprite-coordinate pixels. */
    asic.sprite_x[0] = 96;
    pixels[0] = 0;
    asic_draw_sprites_char(&asic, 6, 6, 0, 40, pixels);
    assert(pixels[0] == 0x112233);

    /* Sprite Y comparison is nine-bit: (VCC[5:0] << 3) | VLC[2:0]. */
    asic.sprite_x[0] = 80;
    asic.sprite_y[0] = 51;
    asic.sprite[0][0][0] = 1;
    pixels[0] = pixels[1] = 0;
    asic_draw_sprites_char(&asic, 5, 6, 3, 40, pixels);
    assert(pixels[0] == 0x112233);

    /* There is no vertical clip: rows below the 200-line window draw as long
     * as the beam reaches them (overscan modes such as GNG's 248-line one). */
    asic.sprite_y[0] = 259;
    pixels[0] = 0;
    asic_draw_sprites_char(&asic, 5, 32, 3, 40, pixels);
    assert(pixels[0] == 0x112233);
}

static void test_sprite_monitor_clipping(void) {
    Asic asic = {0};
    u32 pixels[16] = {0};

    asic.palette[17] = 0x112233;
    asic.sprite_mag_x[0] = 1;
    asic.sprite_mag_y[0] = 1;
    asic.sprite[0][0][0] = 1;

    /* Sprite X is the CRTC counter; a sprite at X=64 fills char 4. */
    asic.sprite_x[0] = 64;
    asic.sprite_y[0] = 48;
    asic_draw_sprites_char(&asic, 4, 6, 0, 40, pixels);
    assert(pixels[0] == 0x112233);

    /* Column zero is fully drawn for a sprite at X=0 (no left clip). */
    asic.sprite_x[0] = 0;
    memset(pixels, 0, sizeof(pixels));
    asic_draw_sprites_char(&asic, 0, 6, 0, 40, pixels);
    assert(pixels[0] == 0x112233);

    /* A sprite at X=1 leaves pixel 0 transparent; its first column is at
     * beam_x 1, so it enters exactly at the display left edge. */
    asic.sprite_x[0] = 1;
    memset(pixels, 0, sizeof(pixels));
    asic_draw_sprites_char(&asic, 0, 6, 0, 40, pixels);
    assert(pixels[0] == 0);
    assert(pixels[1] == 0x112233);

    /* The clip follows the CRTC display width (R1): with R1=34, the last
     * drawable sprite column is X=543; X=544 (char 34) is clipped. */
    asic.sprite_x[0] = 543;
    memset(pixels, 0, sizeof(pixels));
    asic_draw_sprites_char(&asic, 33, 6, 0, 34, pixels);
    assert(pixels[15] == 0x112233);

    asic.sprite_x[0] = 544;
    memset(pixels, 0, sizeof(pixels));
    asic_draw_sprites_char(&asic, 34, 6, 0, 34, pixels);
    assert(pixels[0] == 0);

    /* With a 40-char display, X=640 is clipped. */
    asic.sprite_x[0] = 640;
    memset(pixels, 0, sizeof(pixels));
    asic_draw_sprites_char(&asic, 40, 6, 0, 40, pixels);
    assert(pixels[0] == 0);

    /* Vertical extent is not clipped: sprites may draw in the lower border. */
    asic.sprite_x[0] = 64;
    asic.sprite_y[0] = 0;
    memset(pixels, 0, sizeof(pixels));
    asic_draw_sprites_char(&asic, 4, 0, 0, 40, pixels);
    assert(pixels[0] == 0x112233);

    asic.sprite_y[0] = 199;
    asic_draw_sprites_char(&asic, 4, 24, 7, 40, pixels);
    assert(pixels[0] == 0x112233);

    asic.sprite_y[0] = 200;
    memset(pixels, 0, sizeof(pixels));
    asic_draw_sprites_char(&asic, 4, 25, 0, 40, pixels);
    assert(pixels[0] == 0x112233);
}

int main(void) {
    test_palette_and_sprite_registers();
    test_raster_split_and_dma();
    test_vectored_interrupts();
    test_dma_pause_cadence();
    test_sprite_beam_composition();
    test_sprite_monitor_clipping();
    return 0;
}

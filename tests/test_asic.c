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

    asic_register_write(&asic, &ga, &mem, 0x6801, 3);
    asic_register_write(&asic, &ga, &mem, 0x6802, 0x12);
    asic_register_write(&asic, &ga, &mem, 0x6803, 0x34);
    asic_apply_split(&asic, &crtc, 0, 3);
    assert(crtc.ma == 0x1234);

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

    /* IVR bit 0 leaves a DMA request asserted until DCSR acknowledges it. */
    asic_register_write(&asic, &ga, &mem, 0x6805, 0xA1);
    asic_irq_ack(&asic, &mem, &ga);
    assert(asic.dma[1].interrupt);
    assert(ga.interrupt_pending);
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
    asic.sprite[0][0][0] = 1;
    asic.sprite[0][1][0] = 2;

    asic_draw_sprites_char(&asic, 0, 0, 0, pixels);
    assert(pixels[0] == 0x112233 && pixels[1] == 0x112233);
    assert(pixels[2] == 0x445566 && pixels[3] == 0x445566);
    assert(pixels[4] == 0x010203);

    /* Lower sprite numbers have priority; pen zero remains transparent. */
    asic.sprite_mag_x[1] = 1;
    asic.sprite_mag_y[1] = 1;
    asic.sprite[1][0][0] = 2;
    pixels[0] = pixels[1] = 0;
    asic_draw_sprites_char(&asic, 0, 0, 0, pixels);
    assert(pixels[0] == 0x112233 && pixels[1] == 0x112233);

    /* Coordinates wrap, allowing a sprite to begin just left of X=0. */
    asic.sprite_x[0] = 0x3FF;
    asic.sprite[0][1][0] = 1;
    pixels[0] = pixels[1] = 0;
    asic_draw_sprites_char(&asic, 0, 0, 0, pixels);
    assert(pixels[0] == 0x112233 && pixels[1] == 0x112233);

    /* Vertical comparison is (VCC[4:0] << 3) | VLC[2:0]. */
    asic.sprite_x[0] = 0;
    asic.sprite_y[0] = 19;
    asic.sprite[0][0][0] = 1;
    pixels[0] = pixels[1] = 0;
    asic_draw_sprites_char(&asic, 0, 2, 3, pixels);
    assert(pixels[0] == 0x112233 && pixels[1] == 0x112233);
}

int main(void) {
    test_palette_and_sprite_registers();
    test_raster_split_and_dma();
    test_vectored_interrupts();
    test_dma_pause_cadence();
    test_sprite_beam_composition();
    return 0;
}

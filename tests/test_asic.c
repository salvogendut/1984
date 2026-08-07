#include "asic.h"

#include <assert.h>

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
    asic_hsync(&asic, &mem, &psg, &ga);
    asic_hsync(&asic, &mem, &psg, &ga);
    assert(!ga.interrupt_pending);
    asic_hsync(&asic, &mem, &psg, &ga);
    assert(ga.interrupt_pending);

    asic_register_write(&asic, &ga, &mem, 0x6801, 3);
    asic_register_write(&asic, &ga, &mem, 0x6802, 0x12);
    asic_register_write(&asic, &ga, &mem, 0x6803, 0x34);
    asic_apply_split(&asic, &crtc);
    assert(crtc.ma == 0x1234);

    /* A programmable raster replaces legacy GA IRQ requests, but the GA
     * counter itself continues running and wrapping. */
    ga.interrupt_pending = false;
    for (int i = 0; i < 49; i++)
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

int main(void) {
    test_palette_and_sprite_registers();
    test_raster_split_and_dma();
    return 0;
}

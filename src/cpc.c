#include "cpc.h"
#include <stdio.h>
#include <string.h>

/* ---- Z80 bus callbacks ---- */

static u8 bus_mem_read(void *ctx, u16 addr) {
    CPC *cpc = ctx;
    return mem_read(&cpc->mem, addr);
}
static void bus_mem_write(void *ctx, u16 addr, u8 val) {
    CPC *cpc = ctx;
    mem_write(&cpc->mem, addr, val);
}

static u8 bus_io_read(void *ctx, u16 port) {
    CPC *cpc = ctx;
    u8 hi = port >> 8;

    /* CRTC read: A14=0 (hi & ~0x40 → 0xBF area) */
    if (!(hi & 0x40)) {
        u8 func = (port >> 8) & 0x03;
        if (func == 0x03) return crtc_read(&cpc->crtc);
        return 0xFF;
    }
    /* PPI: A11=0 selects PPI (0xF4xx/0xF5xx/0xF6xx/0xF7xx) */
    if (!(hi & 0x08)) {
        return ppi_read(&cpc->ppi, (port >> 8) & 0x03);
    }
    return 0xFF;
}

static void bus_io_write(void *ctx, u16 port, u8 val) {
    CPC *cpc = ctx;
    u8 hi = port >> 8;

    /* Gate Array: A15=0, A14=1 → 0x7Fxx */
    if (!(hi & 0x80) && (hi & 0x40)) {
        ga_write(&cpc->ga, val);
        cpc->mem.lower_rom_enabled = cpc->ga.lower_rom;
        cpc->mem.upper_rom_enabled = cpc->ga.upper_rom;
        /* 6128 RAM banking */
        if ((val & 0xC0) == 0xC0 && cpc->model == MODEL_6128)
            cpc->mem.ram_bank = val & 0x07;
        return;
    }
    /* CRTC: A14=0 → 0xBCxx/0xBDxx */
    if (!(hi & 0x40)) {
        if (!(hi & 0x02)) crtc_select(&cpc->crtc, val);
        else               crtc_write(&cpc->crtc, val);
        return;
    }
    /* PPI: A11=0 → 0xF4–0xF7 range */
    if (!(hi & 0x08)) {
        ppi_write(&cpc->ppi, (hi >> 1) & 0x03, val);
        /* Route PSG control bits from PPI port C */
        u8 psg_ctrl = (cpc->ppi.port_c >> 6) & 0x03;
        if (psg_ctrl == 0x03) psg_select(&cpc->psg, cpc->ppi.port_a);
        else if (psg_ctrl == 0x02) psg_write(&cpc->psg, cpc->ppi.port_a);
        else if (psg_ctrl == 0x01) cpc->ppi.port_a = psg_read(&cpc->psg);
        /* Feed keyboard row into PSG port A */
        psg_set_kbd_row(&cpc->psg, kbd_read_row(&cpc->kbd, cpc->ppi.kbd_row));
        return;
    }
}

/* ---- Init / destroy ---- */

int cpc_init(CPC *cpc, CpcModel model, const char *rom_os, const char *rom_basic) {
    memset(cpc, 0, sizeof(*cpc));
    cpc->model = model;
    cpc->cpu_clk_hz = 4000000;
    /* 50 Hz PAL: 4 MHz / 50 = 80 000 cycles per frame */
    cpc->cycles_per_frame = cpc->cpu_clk_hz / 50;

    mem_init(&cpc->mem);
    if (mem_load_rom(&cpc->mem, rom_os, rom_basic) < 0)
        return -1;

    z80_init(&cpc->cpu);
    z80_reset(&cpc->cpu);

    ga_init(&cpc->ga);
    crtc_init(&cpc->crtc);
    ppi_init(&cpc->ppi);
    psg_init(&cpc->psg);
    kbd_init(&cpc->kbd);

    cpc->bus.mem_read  = bus_mem_read;
    cpc->bus.mem_write = bus_mem_write;
    cpc->bus.io_read   = bus_io_read;
    cpc->bus.io_write  = bus_io_write;
    cpc->bus.ctx       = cpc;

    const char *title = (model == MODEL_464) ? "CPC 464" : "CPC 6128";
    if (display_init(&cpc->display, title) < 0)
        return -1;

    return 0;
}

void cpc_destroy(CPC *cpc) {
    display_destroy(&cpc->display);
}

/* ---- Frame execution ---- */

void cpc_frame(CPC *cpc) {
    int target = cpc->cycles_per_frame + cpc->cycle_debt;
    int done   = 0;

    while (done < target) {
        int t = z80_step(&cpc->cpu, &cpc->bus);
        done += t;

        /* CRTC ticks at 1 MHz = every 4 CPU cycles */
        for (int i = 0; i < t; i += 4) {
            crtc_tick(&cpc->crtc);

            /* HSYNC → Gate Array interrupt counter */
            if (crtc_hsync(&cpc->crtc))
                ga_hsync(&cpc->ga);

            /* VSYNC → PPI port B bit 0 */
            ppi_set_vsync(&cpc->ppi, crtc_vsync(&cpc->crtc));

            /* Render a Gate Array pixel during display enable */
            if (crtc_de(&cpc->crtc)) {
                u16 ma   = crtc_screen_addr(&cpc->crtc);
                u8  byte = mem_read(&cpc->mem, 0xC000 + (ma & 0x3FFF));
                /* Simplified: just draw border colour outside, ink 0/1 inside */
                u8  pen  = (byte & 0x80) ? 1 : 0;
                u32 rgb  = ga_hw_palette[cpc->ga.ink[pen]];
                display_put_pixel(&cpc->display, rgb);
            } else {
                u32 border = ga_hw_palette[cpc->ga.ink[16]];
                display_put_pixel(&cpc->display, border);
            }

            if (crtc_hsync(&cpc->crtc))
                display_next_line(&cpc->display);

            if (crtc_vsync(&cpc->crtc))
                display_vsync(&cpc->display);
        }

        /* Deliver pending Gate Array interrupt */
        if (cpc->ga.interrupt_pending) {
            cpc->ga.interrupt_pending = false;
            z80_interrupt(&cpc->cpu);
        }
    }

    cpc->cycle_debt = done - target;
    display_present(&cpc->display);
}

void cpc_key_event(CPC *cpc, SDL_Scancode sc, bool pressed) {
    kbd_sdl_key(&cpc->kbd, sc, pressed);
}

#include "ppi.h"
#include <string.h>

void ppi_init(PPI *p) {
    memset(p, 0, sizeof(*p));
    /* An 8255 reset makes every port an input. The CPC firmware subsequently
     * programs 0x82 (A/C output, B input) during machine startup. */
    p->control = 0x9B;
    p->input_a = 0xFF;
}

void ppi_refresh_outputs(PPI *p) {
    p->kbd_row = (p->control & 0x01) ? 0 : (p->port_c & 0x0F);
}

void ppi_write(PPI *p, u8 port, u8 val) {
    switch (port & 0x03) {
        case 0:
            if (!(p->control & 0x10)) p->port_a = val;
            break;
        case 1:
            if (!(p->control & 0x02)) p->port_b = val;
            break;
        case 2:
            p->port_c = val;
            ppi_refresh_outputs(p);
            break;
        case 3:
            if (val & 0x80) {
                p->control = val;
                /* An 8255 mode-set control word clears all output latches. */
                p->port_a = 0;
                p->port_b = 0;
                p->port_c = 0;
                ppi_refresh_outputs(p);
            } else {
                /* bit set/reset mode for port C */
                u8 bit = (val >> 1) & 0x07;
                if (val & 0x01)
                    p->port_c |= (1 << bit);
                else
                    p->port_c &= ~(1 << bit);
                ppi_refresh_outputs(p);
            }
            break;
    }
}

u8 ppi_read(PPI *p, u8 port) {
    switch (port & 0x03) {
        case 0:
            return (p->control & 0x10) ? p->input_a : p->port_a;
        case 1: {
            if (!(p->control & 0x02)) return p->port_b;
            /* bit 7: cassette data input; bit 6: printer BUSY (active
             * high — firmware's MC BUSY PRINTER rotates this bit into
             * carry and treats carry=1 as busy);
             * bits 4-1: jumpers (0x1E = 50Hz PAL, no expansion);
             * bit 0: VSYNC.
             * We hold BUSY=0 so the firmware printer routine always
             * proceeds to the OUT (&EFxx),A write — the host-side
             * capture happens in src/printer.c. */
            u8 b = 0x1E;
            if (p->vsync_signal) b |= 0x01;
            b |= (p->tape_level & 0x80);
            return b;
        }
        case 2:
            return ppi_output_c(p);
        default:
            return p->control;
    }
}

u8 ppi_output_c(const PPI *p) {
    u8 pins = 0;
    if (!(p->control & 0x08)) pins |= p->port_c & 0xF0;
    if (!(p->control & 0x01)) pins |= p->port_c & 0x0F;
    return pins;
}

void ppi_set_port_a_input(PPI *p, u8 value) {
    p->input_a = value;
}

void ppi_set_vsync(PPI *p, bool v) {
    p->vsync_signal = v;
}

void ppi_set_tape_level(PPI *p, u8 level) {
    p->tape_level = level & 0x80;
}

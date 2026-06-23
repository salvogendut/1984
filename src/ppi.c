#include "ppi.h"
#include <string.h>

void ppi_init(PPI *p) {
    memset(p, 0, sizeof(*p));
    p->control = 0x82;   /* standard CPC init: A=input, B=input, C=output */
}

void ppi_write(PPI *p, u8 port, u8 val) {
    switch (port & 0x03) {
        case 0: p->port_a = val; break;
        case 1: p->port_b = val; break;
        case 2:
            p->port_c = val;
            p->kbd_row = val & 0x0F;
            break;
        case 3:
            if (val & 0x80) {
                p->control = val;
                /* An 8255 mode-set control word clears all output latches. */
                p->port_a = 0;
                p->port_b = 0;
                p->port_c = 0;
                p->kbd_row = 0;
            } else {
                /* bit set/reset mode for port C */
                u8 bit = (val >> 1) & 0x07;
                if (val & 0x01)
                    p->port_c |= (1 << bit);
                else
                    p->port_c &= ~(1 << bit);
                p->kbd_row = p->port_c & 0x0F;
            }
            break;
    }
}

u8 ppi_read(PPI *p, u8 port) {
    switch (port & 0x03) {
        case 0: return p->port_a;
        case 1: {
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
        case 2: return p->port_c;
        default: return 0xFF;
    }
}

void ppi_set_vsync(PPI *p, bool v) {
    p->vsync_signal = v;
}

void ppi_set_tape_level(PPI *p, u8 level) {
    p->tape_level = level & 0x80;
}

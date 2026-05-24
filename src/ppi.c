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
            u8 b = 0xFE; /* open-collector pull-up, all bits high when not driven */
            if (p->vsync_signal) b |= 0x01;
            return b;
        }
        case 2: return p->port_c;
        default: return 0xFF;
    }
}

void ppi_set_vsync(PPI *p, bool v) {
    p->vsync_signal = v;
}

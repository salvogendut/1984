#pragma once

#include "types.h"

/* The stock CPC devices use partial address decoding. More than one device
 * can therefore observe the same I/O cycle; callers must not treat this mask
 * as a priority-ordered device number. */
enum {
    CPC_IO_CRTC       = 1u << 0, /* A14=0 */
    CPC_IO_PPI        = 1u << 1, /* A11=0 */
    CPC_IO_GATE_ARRAY = 1u << 2, /* A15=0, A14=1 */
    CPC_IO_ROM_SELECT = 1u << 3, /* A13=0, writes only */
    CPC_IO_PRINTER    = 1u << 4, /* A12=0, writes only */
};

static inline unsigned cpc_io_decode(u16 port) {
    unsigned selected = 0;
    if (!(port & 0x4000)) selected |= CPC_IO_CRTC;
    if (!(port & 0x0800)) selected |= CPC_IO_PPI;
    if ((port & 0xC000) == 0x4000) selected |= CPC_IO_GATE_ARRAY;
    if (!(port & 0x2000)) selected |= CPC_IO_ROM_SELECT;
    if (!(port & 0x1000)) selected |= CPC_IO_PRINTER;
    return selected;
}

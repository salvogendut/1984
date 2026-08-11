#include "io_decode.h"

#include <assert.h>

static void expect(u16 port, unsigned selected) {
    assert(cpc_io_decode(port) == selected);
}

int main(void) {
    expect(0xBC00, CPC_IO_CRTC);
    expect(0x7F00, CPC_IO_GATE_ARRAY);
    expect(0xDF00, CPC_IO_ROM_SELECT);
    expect(0xF400, CPC_IO_PPI);
    expect(0xEF00, CPC_IO_PRINTER);

    /* Partial decoding permits intentional overlaps. */
    expect(0x7700, CPC_IO_GATE_ARRAY | CPC_IO_PPI);
    expect(0x4000, CPC_IO_GATE_ARRAY | CPC_IO_PPI |
                   CPC_IO_ROM_SELECT | CPC_IO_PRINTER);
    expect(0x0000, CPC_IO_CRTC | CPC_IO_PPI |
                   CPC_IO_ROM_SELECT | CPC_IO_PRINTER);
    return 0;
}

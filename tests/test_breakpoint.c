#include "cpc.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    CPC cpc;
    memset(&cpc, 0, sizeof(cpc));
    cpc.snapshot_breakpoints = true;
    mem_init(&cpc.mem);

    CpcBreakpointId ids[128];
    for (int i = 0; i < 128; i++) {
        ids[i] = cpc_breakpoint_add(&cpc, (u16)(0x1000 + i), CPC_BP_ANY,
                                    0, CPC_BP_SOURCE_DAP);
        assert(ids[i] != CPC_BREAKPOINT_INVALID_ID);
    }
    assert(cpc_breakpoint_count(&cpc) == 128);
    assert(cpc_breakpoint_match(&cpc, 0x107f,
                                CPC_BREAKPOINT_INVALID_ID) == ids[127]);

    cpc_breakpoint_clear(&cpc, ids[20]);
    assert(cpc_breakpoint_count(&cpc) == 127);
    assert(!cpc_breakpoint_get(&cpc, ids[20]));
    assert(cpc_breakpoint_get(&cpc, ids[100])->address == 0x1064);

    CpcBreakpointId duplicate = cpc_breakpoint_add(
        &cpc, 0x1064, CPC_BP_ANY, 0, CPC_BP_SOURCE_DAP);
    assert(duplicate == ids[100]);
    assert(cpc_breakpoint_count(&cpc) == 127);

    CpcBreakpointId user = cpc_breakpoint_add(
        &cpc, 0x1064, CPC_BP_ANY, 0, CPC_BP_SOURCE_USER);
    assert(user != CPC_BREAKPOINT_INVALID_ID && user != ids[100]);
    assert(cpc_breakpoint_match(&cpc, 0x1064, ids[100]) == user);

    CpcBreakpointId snapshot = cpc_breakpoint_add(
        &cpc, 0x3333, CPC_BP_ANY, 0, CPC_BP_SOURCE_SNAPSHOT);
    CpcBreakpointId dap = cpc_breakpoint_add(
        &cpc, 0x3333, CPC_BP_ANY, 0, CPC_BP_SOURCE_DAP);
    assert(snapshot != CPC_BREAKPOINT_INVALID_ID &&
           dap != CPC_BREAKPOINT_INVALID_ID && snapshot != dap);
    cpc_set_snapshot_breakpoints(&cpc, false);
    assert(!cpc_breakpoint_get(&cpc, snapshot)->armed);
    assert(cpc_breakpoint_get(&cpc, dap)->armed);
    assert(cpc_breakpoint_match(&cpc, 0x3333,
                                CPC_BREAKPOINT_INVALID_ID) == dap);
    cpc_set_snapshot_breakpoints(&cpc, true);
    assert(cpc_breakpoint_get(&cpc, snapshot)->armed);

    cpc_breakpoint_clear_source(&cpc, CPC_BP_SOURCE_DAP);
    assert(!cpc_breakpoint_get(&cpc, ids[100]));
    assert(!cpc_breakpoint_get(&cpc, dap));
    assert(cpc_breakpoint_get(&cpc, user));
    assert(cpc_breakpoint_get(&cpc, snapshot));

    cpc_breakpoints_destroy(&cpc);
    puts("dynamic breakpoint tests passed");
    return 0;
}

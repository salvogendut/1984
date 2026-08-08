#include "../src/cpc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_gx4000_capabilities(void) {
    assert(cpc_model_is_plus(MODEL_GX4000));
    assert(cpc_model_is_console(MODEL_GX4000));
    assert(!cpc_model_is_128k(MODEL_GX4000));
    assert(!cpc_model_has_builtin_fdc(MODEL_GX4000));
    assert(!cpc_model_has_builtin_tape(MODEL_GX4000));
    assert(!cpc_model_has_keyboard(MODEL_GX4000));
    assert(strcmp(cpc_model_name(MODEL_GX4000), "GX4000") == 0);
    assert(strcmp(cpc_model_config_name(MODEL_GX4000), "gx4000") == 0);
}

static void test_computer_model_regressions(void) {
    assert(cpc_model_has_keyboard(MODEL_464_PLUS));
    assert(cpc_model_has_builtin_tape(MODEL_464_PLUS));
    assert(cpc_model_is_128k(MODEL_6128_PLUS));
    assert(cpc_model_has_builtin_fdc(MODEL_6128_PLUS));
    assert(!cpc_model_is_console(MODEL_6128_PLUS));
}

int main(void) {
    test_gx4000_capabilities();
    test_computer_model_regressions();
    puts("model tests passed");
    return 0;
}

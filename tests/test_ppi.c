#include "ppi.h"

#include <assert.h>

static void test_reset_and_direction(void) {
    PPI ppi;
    ppi_init(&ppi);

    assert(ppi.control == 0x9B);
    assert(ppi_read(&ppi, 0) == 0xFF);
    assert(ppi_read(&ppi, 1) == 0x1E);
    assert(ppi_read(&ppi, 2) == 0x00);
    assert(ppi_read(&ppi, 3) == 0x9B);

    /* Writes to input-configured A/B ports do not change their latches. */
    ppi_write(&ppi, 0, 0x12);
    ppi_write(&ppi, 1, 0x34);
    assert(ppi.port_a == 0);
    assert(ppi.port_b == 0);

    /* Firmware mode: A/C output, B input. */
    ppi_write(&ppi, 3, 0x82);
    ppi_write(&ppi, 0, 0x56);
    ppi_write(&ppi, 1, 0x78);
    ppi_write(&ppi, 2, 0xA9);
    assert(ppi_read(&ppi, 0) == 0x56);
    assert(ppi_read(&ppi, 1) == 0x1E);
    assert(ppi_read(&ppi, 2) == 0xA9);
    assert(ppi_output_c(&ppi) == 0xA9);
    assert(ppi.kbd_row == 9);
}

static void test_split_port_c_and_input_a(void) {
    PPI ppi;
    ppi_init(&ppi);
    ppi_set_port_a_input(&ppi, 0x5A);
    assert(ppi_read(&ppi, 0) == 0x5A);

    /* Upper C input, lower C output. */
    ppi_write(&ppi, 3, 0x88);
    ppi_write(&ppi, 2, 0xF5);
    assert(ppi_read(&ppi, 2) == 0x05);
    assert(ppi_output_c(&ppi) == 0x05);
    assert(ppi.kbd_row == 5);

    ppi_write(&ppi, 3, 0x0D); /* set port C bit 6 */
    assert(ppi.port_c == 0xF5);
    assert(ppi_output_c(&ppi) == 0x05);
}

int main(void) {
    test_reset_and_direction();
    test_split_port_c_and_input_a();
    return 0;
}

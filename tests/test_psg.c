#include "psg.h"

#include <assert.h>
#include <string.h>

static void test_register_write_masks(void) {
    PSG psg;
    psg_init(&psg);

    psg_select(&psg, 1);
    psg_write(&psg, 0xFF);
    assert(psg.reg[1] == 0x0F);

    psg_select(&psg, 6);
    psg_write(&psg, 0xFF);
    assert(psg.reg[6] == 0x1F);

    psg_select(&psg, 7);
    psg_write(&psg, 0xFF);
    assert(psg.reg[7] == 0x3F);

    psg_select(&psg, 8);
    psg_write(&psg, 0xFF);
    assert(psg.reg[8] == 0x1F);

    psg_select(&psg, 13);
    psg_write(&psg, 0xFF);
    assert(psg.reg[13] == 0x0F);
    assert(psg.env_step == 0);
    assert(!psg.env_hold);
    assert(psg.env_dir);
}

static void test_snapshot_register_load_and_store(void) {
    PSG psg;
    u8 regs[PSG_NUM_REGS];
    u8 stored[PSG_NUM_REGS];
    u8 selected = 0;
    u8 env_step = 0;
    u8 env_direction = 0;

    memset(regs, 0xFF, sizeof(regs));
    psg_init(&psg);
    psg_set_kbd_row(&psg, 0x5A);

    psg_load_registers(&psg, regs, 9, true, 12, 0xFF);

    assert(psg.selected == 9);
    assert(psg.kbd_data == 0x5A);
    assert(psg.noise_lfsr == 1);
    assert(psg.reg[0] == 0xFF);
    assert(psg.reg[1] == 0x0F);
    assert(psg.reg[6] == 0x1F);
    assert(psg.reg[7] == 0x3F);
    assert(psg.reg[8] == 0x1F);
    assert(psg.reg[13] == 0x0F);
    assert(!psg.env_dir);
    assert(!psg.env_hold);
    assert(psg.env_step == 7);

    psg_store_registers(&psg, stored, &selected, &env_step, &env_direction);
    assert(selected == 9);
    assert(memcmp(stored, psg.reg, sizeof(stored)) == 0);
    assert(env_step == 12);
    assert(env_direction == 0xFF);
}

static void test_snapshot_held_envelope_level(void) {
    PSG psg;
    u8 regs[PSG_NUM_REGS] = {0};
    u8 env_step = 0;
    u8 env_direction = 0xFF;

    psg_init(&psg);
    psg_load_registers(&psg, regs, 0, true, 4, 0x00);

    assert(psg.env_hold);
    assert(psg.env_dir);
    assert(psg.env_step == 8);

    psg_store_registers(&psg, regs, NULL, &env_step, &env_direction);
    assert(env_step == 4);
    assert(env_direction == 0x00);
}

static void test_disabled_sources_can_act_as_volume_dac(void) {
    PSG psg;
    s16 buf[32] = {0};
    int heard = 0;

    psg_init(&psg);
    psg_set_volume(&psg, 100);
    psg_select(&psg, 7);
    psg_write(&psg, 0x3F);   /* disable tone and noise on all channels */
    psg_select(&psg, 8);
    psg_write(&psg, 0x00);
    psg_render_stereo(&psg, buf, 8, 1000000, 44100);

    psg_select(&psg, 8);
    psg_write(&psg, 0x0F);   /* volume-register DAC step */
    memset(buf, 0, sizeof(buf));
    psg_render_stereo(&psg, buf, 8, 1000000, 44100);
    for (int i = 0; i < 16; i++) {
        if (buf[i] != 0)
            heard = 1;
    }
    assert(heard);
}

int main(void) {
    test_register_write_masks();
    test_snapshot_register_load_and_store();
    test_snapshot_held_envelope_level();
    test_disabled_sources_can_act_as_volume_dac();
    return 0;
}

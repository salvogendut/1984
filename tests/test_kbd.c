/* 1984 Amstrad CPC emulator
 * Copyright (c) 2026 Salvatore Bognanni
 * Copyright (c) 2026 Nick J. Date
 * Licensed under the GNU General Public License, version 2 (see LICENSE).
 */
#include "../src/kbd.h"

#include <assert.h>
#include <stdio.h>

static void test_punctuation_keys(void)
{
    Keyboard k;

    kbd_init(&k);

    /* The first physical key after L is colon on a UK CPC keyboard. */
    assert(kbd_sdl_key(&k, SDL_SCANCODE_SEMICOLON, true));
    assert(kbd_read_row(&k, 3) == 0xDF);
    assert(kbd_sdl_key(&k, SDL_SCANCODE_SEMICOLON, false));
    assert(kbd_read_row(&k, 3) == 0xFF);

    /* The second physical key after L is semicolon on a UK CPC keyboard. */
    assert(kbd_sdl_key(&k, SDL_SCANCODE_APOSTROPHE, true));
    assert(kbd_read_row(&k, 3) == 0xEF);
    assert(kbd_sdl_key(&k, SDL_SCANCODE_APOSTROPHE, false));
    assert(kbd_read_row(&k, 3) == 0xFF);
}

static KeyScript ks;   /* static: a few KB */

static void test_keyscript_parse_errors(void)
{
    assert(!keyscript_init(&ks, "Q+NOSUCHKEY:2"));
    assert(ks.done && ks.nsteps == 0);
    assert(!keyscript_init(&ks, "G:5x"));
    assert(!keyscript_init(&ks, "G+:5"));
    assert(!keyscript_init(&ks, ""));
}

static void test_keyscript_timeline(void)
{
    Keyboard k;

    kbd_init(&k);
    /* "n" is the N key here, not "neutral" as in --joy-script. */
    assert(keyscript_init(&ks, "-:2,g+shift:1,N,1:3"));
    assert(ks.nsteps == 4);

    /* Frames 1-2: nothing pressed. */
    keyscript_tick(&ks, &k);
    keyscript_tick(&ks, &k);
    for (int r = 0; r < KBD_ROWS; r++) assert(kbd_read_row(&k, r) == 0xFF);

    /* Frame 3: G (row 6 bit 4) and Shift (row 2 bit 5). */
    keyscript_tick(&ks, &k);
    assert(kbd_read_row(&k, 6) == 0xEF);
    assert(kbd_read_row(&k, 2) == 0xDF);

    /* Frame 4: N (row 5 bit 6) alone; G and Shift released. */
    keyscript_tick(&ks, &k);
    assert(kbd_read_row(&k, 5) == 0xBF);
    assert(kbd_read_row(&k, 6) == 0xFF);
    assert(kbd_read_row(&k, 2) == 0xFF);

    /* Keys the script never names are left alone: hold Q (row 8 bit 3)
     * as if from the host keyboard. */
    kbd_key_down(&k, 8, 3);

    /* Frames 5-7: 1 (row 8 bit 0); Q stays down. */
    for (int f = 0; f < 3; f++) {
        assert(!ks.done);
        keyscript_tick(&ks, &k);
        assert(kbd_read_row(&k, 8) == 0xF6);
        assert(kbd_read_row(&k, 5) == 0xFF);
    }

    /* Frame 8: the script has ended; 1 released, Q untouched. */
    assert(!ks.done);
    keyscript_tick(&ks, &k);
    assert(ks.done);
    assert(kbd_read_row(&k, 8) == 0xF7);

    /* Further ticks do nothing. */
    keyscript_tick(&ks, &k);
    assert(kbd_read_row(&k, 8) == 0xF7);
}

static void test_keyscript_last_step_seen(void)
{
    Keyboard k;

    /* A 1-frame last step is held for its frame and released the next. */
    kbd_init(&k);
    assert(keyscript_init(&ks, "DEL+ESC:1"));
    keyscript_tick(&ks, &k);
    assert(!ks.done);
    assert(kbd_read_row(&k, 9) == 0x7F);
    assert(kbd_read_row(&k, 8) == 0xFB);
    keyscript_tick(&ks, &k);
    assert(ks.done);
    assert(kbd_read_row(&k, 9) == 0xFF);
    assert(kbd_read_row(&k, 8) == 0xFF);
}

int main(void)
{
    test_punctuation_keys();
    test_keyscript_parse_errors();
    test_keyscript_timeline();
    test_keyscript_last_step_seen();
    puts("kbd tests passed");
    return 0;
}

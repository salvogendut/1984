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

int main(void)
{
    test_punctuation_keys();
    puts("kbd tests passed");
    return 0;
}

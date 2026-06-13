#pragma once
#include <stdbool.h>
#include "kbd.h"

typedef struct {
    char *buf;      /* heap-allocated clipboard text */
    int   len;
    int   pos;      /* index of next character to inject */
    int   timer;    /* frames to wait before next action */
    bool  held;     /* true while the current key is pressed */
} Paste;

void paste_init(Paste *p);
void paste_free(Paste *p);

/* Queue text for character-by-character injection into the CPC keyboard.
 * Auto-appends a newline so single-shot commands fire on their own. */
void paste_text(Paste *p, const char *text);

/* Like paste_text but does NOT append a newline. Used by the kbd PTY to
 * stream incoming PTY bytes verbatim — the caller is responsible for
 * sending \r itself when it wants Enter pressed. */
void paste_text_raw(Paste *p, const char *text);

/* Call once per frame before cpc_frame(); injects one key event at a time. */
void paste_tick(Paste *p, Keyboard *k);

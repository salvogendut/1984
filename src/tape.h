#pragma once
#include "types.h"
#include <stdbool.h>
#include <stddef.h>

/* Compact CDT (TZX) tape image decoder.
 *
 * Drives PPI Port B bit 7 (cassette data input) from a CDT-format
 * tape image while the cassette motor is on. The cycle-accurate level
 * transitions are produced by tape_step(), which the Z80 step loop
 * calls every instruction with the instruction's T-state count.
 *
 * Supported CDT block types: 0x10 standard speed data, 0x11 turbo
 * loading data, 0x12 pure tone, 0x13 pulse sequence, 0x14 pure data,
 * 0x20 pause. Metadata blocks 0x21/22/30/31/32/33/34/5A are skipped.
 * Unknown blocks fall back to a generic skip; if that overshoots the
 * decoder simply hits TAPE_END and the tape stops. */

typedef enum {
    TAPE_END = 0,
    TAPE_PILOT,
    TAPE_SYNC,
    TAPE_DATA,
    TAPE_PAUSE,
} TapeStage;

typedef struct {
    u8     *image;          /* malloc()ed CDT bytes */
    size_t  image_size;
    u8     *block;          /* current block start */
    u8     *block_end;      /* image + image_size */
    u8     *data_p;         /* into current block's data area */

    bool    present;        /* image is loaded */
    bool    motor;          /* PPI Port C bit 4 mirror */
    u8      level;          /* 0x00 or 0x80, OR'd into PPI Port B bit 7 */

    TapeStage stage;
    int     cycles_until_next;   /* counts down via tape_step() */
    u32     pulse_cycles;        /* cycles per pulse at this stage */
    u32     zero_pulse_cycles;
    u32     one_pulse_cycles;
    u32     pulse_count;
    u32     data_bits;           /* remaining data bits in current block */
    u32     bits_to_shift;
    u8      data_byte;

    /* Type-13 pulse sequence: pointer into the block's pulse table */
    u16    *pulse_table;
    u16    *pulse_table_end;
    u16    *pulse_table_ptr;
    /* Type-10 sync uses these two embedded cells */
    u16     sync_table[2];
} Tape;

void tape_init(Tape *t);
bool tape_load(Tape *t, const char *path);    /* false on I/O error */
void tape_eject(Tape *t);
bool tape_loaded(const Tape *t);

void tape_rewind(Tape *t);                    /* restart from first block */
void tape_set_motor(Tape *t, bool on);

/* Call from the Z80 step loop with the instruction's T-state count.
 * Advances the tape state machine when motor is on. */
void tape_step(Tape *t, int cycles);

/* 0x00 or 0x80 — OR into PPI Port B during read. */
u8   tape_level(const Tape *t);

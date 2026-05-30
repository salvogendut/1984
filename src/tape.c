#include "tape.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* CDT (TZX) decoder, ported in compact form from Caprice32's tape.cpp.
 * The pulse timings in a CDT file are expressed in Spectrum T-states
 * (3.5 MHz). We scale up to the CPC's 4 MHz clock: 40/35 = 8/7. */
#define CYCLE_SCALE(p)  (((u32)(p) * 40u + 17u) / 35u)
#define MS_TO_CYCLES(p) ((u32)(p) * 4000u)

#define TAPE_LEVEL_LOW   0x00
#define TAPE_LEVEL_HIGH  0x80

/* Little-endian unaligned readers (CDT is x86-byte-order). */
static u16 rd16(const u8 *p) { return (u16)p[0] | ((u16)p[1] << 8); }
static u32 rd24(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16);
}
static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static void switch_level(Tape *t) {
    t->level = (t->level == TAPE_LEVEL_HIGH) ? TAPE_LEVEL_LOW : TAPE_LEVEL_HIGH;
}

static bool read_data_bit(Tape *t) {
    if (!t->data_bits) return false;
    if (!t->bits_to_shift) {
        t->data_byte = *t->data_p++;
        t->bits_to_shift = 8;
    }
    u8 bit = t->data_byte & 0x80;
    t->data_byte <<= 1;
    t->bits_to_shift--;
    t->data_bits--;
    t->pulse_cycles = bit ? t->one_pulse_cycles : t->zero_pulse_cycles;
    t->pulse_count = 2;   /* two pulses per data bit */
    return true;
}

/* Advance to the next block; returns true if a playable block was set
 * up, false if we ran off the end. */
static bool next_block(Tape *t) {
    while (t->block < t->block_end) {
        u8 id = *t->block;
        switch (id) {

        case 0x10: /* standard speed data block */
            t->stage = TAPE_PILOT;
            t->pulse_cycles = CYCLE_SCALE(2168);
            t->cycles_until_next += (int)t->pulse_cycles;
            t->pulse_count = 3220;
            return true;

        case 0x11: /* turbo loading data block */
            t->stage = TAPE_PILOT;
            t->pulse_cycles = CYCLE_SCALE(rd16(t->block + 0x01));
            t->cycles_until_next += (int)t->pulse_cycles;
            t->pulse_count = rd16(t->block + 0x01 + 0x0A);
            return true;

        case 0x12: /* pure tone */
            t->stage = TAPE_PILOT;
            t->pulse_cycles = CYCLE_SCALE(rd16(t->block + 0x01));
            t->cycles_until_next += (int)t->pulse_cycles;
            t->pulse_count = rd16(t->block + 0x01 + 0x02);
            return true;

        case 0x13: /* sequence of pulses */
            t->stage = TAPE_SYNC;
            t->pulse_count = *(t->block + 0x01);
            t->pulse_table     = (u16 *)(t->block + 0x01 + 0x01);
            t->pulse_table_ptr = t->pulse_table;
            t->pulse_table_end = t->pulse_table + t->pulse_count;
            t->pulse_cycles    = CYCLE_SCALE(*t->pulse_table_ptr++);
            t->cycles_until_next += (int)t->pulse_cycles;
            return true;

        case 0x14: /* pure data block */
            t->stage = TAPE_DATA;
            t->zero_pulse_cycles = CYCLE_SCALE(rd16(t->block + 0x01));
            t->one_pulse_cycles  = CYCLE_SCALE(rd16(t->block + 0x01 + 0x02));
            {
                u32 bytes = rd24(t->block + 0x01 + 0x07);
                t->data_bits = (bytes - 1) * 8u + *(t->block + 0x01 + 0x04);
            }
            t->data_p = t->block + 0x01 + 0x0A;
            t->bits_to_shift = 0;
            read_data_bit(t);
            t->cycles_until_next += (int)t->pulse_cycles;
            return true;

        case 0x20: /* pause */
            if (rd16(t->block + 0x01)) {
                t->stage = TAPE_PAUSE;
                t->pulse_cycles = MS_TO_CYCLES(1);
                t->cycles_until_next += (int)t->pulse_cycles;
                t->pulse_cycles = MS_TO_CYCLES(rd16(t->block + 0x01) - 1);
                t->pulse_count  = 2;
                return true;
            }
            t->block += 2 + 1;   /* zero-length pause: just skip */
            break;

        /* Metadata blocks — skip past them, then re-loop. */
        case 0x21: t->block += *(t->block + 0x01) + 1 + 1; break;
        case 0x22: t->block += 1; break;
        case 0x30: t->block += *(t->block + 0x01) + 1 + 1; break;
        case 0x31: t->block += *(t->block + 0x01 + 0x01) + 2 + 1; break;
        case 0x32: t->block += rd16(t->block + 0x01) + 2 + 1; break;
        case 0x33: t->block += *(t->block + 0x01) * 3 + 1 + 1; break;
        case 0x34: t->block += 8 + 1; break;
        case 0x5A: t->block += 9 + 1; break;

        default: /* "extension rule" — generic skip */
            t->block += rd32(t->block + 0x01) + 4 + 1;
            break;
        }
    }
    return false;
}

/* Move the block pointer past the block we just finished playing,
 * then queue the next block. */
static void block_done(Tape *t) {
    if (t->block >= t->block_end) { t->stage = TAPE_END; return; }
    switch (*t->block) {
    case 0x10: t->block += rd16(t->block + 0x01 + 0x02) + 0x04 + 1; break;
    case 0x11: t->block += rd24(t->block + 0x01 + 0x0F) + 0x12 + 1; break;
    case 0x12: t->block += 4 + 1; break;
    case 0x13: t->block += *(t->block + 0x01) * 2 + 1 + 1; break;
    case 0x14: t->block += rd24(t->block + 0x01 + 0x07) + 0x0A + 1; break;
    case 0x20: t->block += 2 + 1; break;
    default:   t->block = t->block_end; break;
    }
    if (!next_block(t))
        t->stage = TAPE_END;
}

static void update_level(Tape *t) {
    switch (t->stage) {

    case TAPE_PILOT:
        switch_level(t);
        if (--t->pulse_count > 0) {
            t->cycles_until_next += (int)t->pulse_cycles;
            break;
        }
        /* Pilot done — enter SYNC or DATA depending on block type. */
        switch (*t->block) {
        case 0x10: /* standard sync (two embedded values) then data */
            t->stage = TAPE_SYNC;
            t->sync_table[0] = 667;
            t->sync_table[1] = 735;
            t->pulse_table     = t->sync_table;
            t->pulse_table_ptr = t->sync_table;
            t->pulse_table_end = t->sync_table + 2;
            t->pulse_cycles    = CYCLE_SCALE(*t->pulse_table_ptr++);
            t->cycles_until_next += (int)t->pulse_cycles;
            t->pulse_count = 2;
            break;
        case 0x11: /* turbo: sync pulse pair taken from block header */
            t->stage = TAPE_SYNC;
            t->pulse_table     = (u16 *)(t->block + 0x01 + 0x02);
            t->pulse_table_ptr = t->pulse_table;
            t->pulse_table_end = t->pulse_table + 2;
            t->pulse_cycles    = CYCLE_SCALE(*t->pulse_table_ptr++);
            t->cycles_until_next += (int)t->pulse_cycles;
            t->pulse_count = 2;
            break;
        case 0x12: /* pure tone: no data afterwards, block done */
            block_done(t);
            break;
        }
        break;

    case TAPE_SYNC:
        switch_level(t);
        if (--t->pulse_count > 0) {
            if (t->pulse_table_ptr >= t->pulse_table_end)
                t->pulse_table_ptr = t->pulse_table;
            t->pulse_cycles = CYCLE_SCALE(*t->pulse_table_ptr++);
            t->cycles_until_next += (int)t->pulse_cycles;
            break;
        }
        switch (*t->block) {
        case 0x10: /* enter DATA with std timings */
            t->stage = TAPE_DATA;
            t->zero_pulse_cycles = CYCLE_SCALE(855);
            t->one_pulse_cycles  = CYCLE_SCALE(1710);
            t->data_bits = (u32)rd16(t->block + 0x01 + 0x02) * 8u;
            t->data_p    = t->block + 0x01 + 0x04;
            t->bits_to_shift = 0;
            read_data_bit(t);
            t->cycles_until_next += (int)t->pulse_cycles;
            break;
        case 0x11: /* turbo data */
            t->stage = TAPE_DATA;
            t->zero_pulse_cycles = CYCLE_SCALE(rd16(t->block + 0x01 + 0x06));
            t->one_pulse_cycles  = CYCLE_SCALE(rd16(t->block + 0x01 + 0x08));
            {
                u32 bytes = rd24(t->block + 0x01 + 0x0F);
                t->data_bits = (bytes - 1) * 8u + *(t->block + 0x01 + 0x0C);
            }
            t->data_p = t->block + 0x01 + 0x12;
            t->bits_to_shift = 0;
            read_data_bit(t);
            t->cycles_until_next += (int)t->pulse_cycles;
            break;
        case 0x13: /* pulse sequence: nothing more, block done */
            block_done(t);
            break;
        }
        break;

    case TAPE_DATA:
        switch_level(t);
        if (--t->pulse_count > 0) {
            t->cycles_until_next += (int)t->pulse_cycles;
            break;
        }
        if (read_data_bit(t)) {
            t->cycles_until_next += (int)t->pulse_cycles;
            break;
        }
        /* End of data: pause requested? */
        {
            u32 pause_ms = 0;
            switch (*t->block) {
            case 0x10: pause_ms = rd16(t->block + 0x01); break;
            case 0x11: pause_ms = rd16(t->block + 0x01 + 0x0D); break;
            case 0x14: pause_ms = rd16(t->block + 0x01 + 0x05); break;
            }
            if (pause_ms) {
                t->stage = TAPE_PAUSE;
                t->pulse_cycles = MS_TO_CYCLES(1);
                t->cycles_until_next += (int)t->pulse_cycles;
                t->pulse_cycles = MS_TO_CYCLES(pause_ms - 1);
                t->pulse_count  = 2;
            } else {
                block_done(t);
            }
        }
        break;

    case TAPE_PAUSE:
        t->level = TAPE_LEVEL_LOW;
        if (--t->pulse_count > 0) {
            t->cycles_until_next += (int)t->pulse_cycles;
        } else {
            block_done(t);
        }
        break;

    case TAPE_END:
        /* tape stops; nothing to do until rewind/reload. */
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */

void tape_init(Tape *t) {
    memset(t, 0, sizeof(*t));
}

bool tape_load(Tape *t, const char *path) {
    tape_eject(t);
    if (!path || !*path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return false; }
    u8 *buf = malloc((size_t)n);
    if (!buf) { fclose(f); return false; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return false;
    }
    fclose(f);

    t->image      = buf;
    t->image_size = (size_t)n;
    t->present    = true;

    /* CDT format: starts with "ZXTape!\x1A" + 2 bytes of version
     * (major + minor), total 10 bytes. Block stream follows.
     * TZX files lacking the signature get treated the same — we just
     * point at byte 0 and let the dispatch sort it out. */
    if (t->image_size >= 10 && memcmp(t->image, "ZXTape!\x1A", 8) == 0)
        t->block = t->image + 10;
    else
        t->block = t->image;
    t->block_end = t->image + t->image_size;

    tape_rewind(t);
    return true;
}

void tape_eject(Tape *t) {
    if (t->image) free(t->image);
    t->image = NULL;
    t->image_size = 0;
    t->present = false;
    t->stage = TAPE_END;
    t->level = TAPE_LEVEL_LOW;
}

bool tape_loaded(const Tape *t) { return t->present; }

void tape_rewind(Tape *t) {
    if (!t->present) return;
    /* Re-seek to the first block after the header (if any). */
    if (t->image_size >= 10 && memcmp(t->image, "ZXTape!\x1A", 8) == 0)
        t->block = t->image + 10;
    else
        t->block = t->image;
    t->level = TAPE_LEVEL_LOW;
    t->cycles_until_next = 0;
    t->stage = TAPE_END;
    t->data_bits = 0;
    t->bits_to_shift = 0;
    t->pulse_count = 0;
    if (!next_block(t)) t->stage = TAPE_END;
}

void tape_set_motor(Tape *t, bool on) {
    t->motor = on;
}

void tape_step(Tape *t, int cycles) {
    if (!t->present || !t->motor || t->stage == TAPE_END) return;
    t->cycles_until_next -= cycles;
    while (t->cycles_until_next <= 0 && t->stage != TAPE_END) {
        update_level(t);
    }
}

u8 tape_level(const Tape *t) { return t->level; }

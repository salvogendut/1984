/* symbols — debugger symbol-table import.
 *
 * Loads SDCC (and later SCC / FuzixCompilerKit) .map files and lets the
 * monitor + disassembler annotate addresses with the closest preceding
 * symbol. Supports per-map MMR matching so the same address in a banked
 * OS (FUZIX) can resolve to different symbols depending on which bank
 * is live. See docs/SYMBOLS_SCOPE.md for the design.
 *
 * Calling `symbols_lookup()` / `symbols_format()` is cheap when nothing
 * is loaded — they short-circuit on an empty registry, so production
 * runs without --symbols pay only a single pointer check per call.
 */
#pragma once

#include "types.h"
#include <stdbool.h>
#include <stddef.h>

/* MMR-match wildcard: symbol applies regardless of the current ram_bank. */
#define SYMBOLS_ANY_MMR  (-1)

typedef struct {
    u16   addr;
    char *name;
    char *module;     /* may be NULL */
} Symbol;

/* Load a .map file. Auto-detects SDCC vs SCC by signature. Returns 0 on
 * success, -1 on I/O or parse error (message to stderr). Symbols are
 * accumulated; multiple calls layer maps in load order. `mmr_match` may
 * be SYMBOLS_ANY_MMR or a specific ram_bank value (0..0xFF). */
int symbols_load(const char *path, int mmr_match);

/* Best symbol for `addr` given current `ram_bank`. Returns NULL when:
 *  - nothing is loaded
 *  - no symbol exists with the right MMR in the closing-cap window
 *    (`max_offset`; pass 0 for the default of 0x100)
 * Result is owned by the symbol table; do not free. */
const Symbol *symbols_lookup(u16 addr, u8 ram_bank, u16 max_offset);

/* Exact name match across all loaded maps. */
const Symbol *symbols_lookup_name(const char *name);

/* Convenience: write "symname" or "symname+0xNN" into `out` for the
 * given (addr, ram_bank). Writes empty string when no match. Always
 * NUL-terminates if out_sz > 0. */
void symbols_format(u16 addr, u8 ram_bank, char *out, size_t out_sz);

/* True iff at least one symbol is loaded. Lets call sites cheaply
 * skip the whole annotation path. */
bool symbols_any_loaded(void);

/* Free all loaded symbols. Mostly for tests; not called in production. */
void symbols_shutdown(void);

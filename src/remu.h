#pragma once

#include "types.h"
#include <stddef.h>

typedef enum {
    REMU_SYMBOL_RAM,
    REMU_SYMBOL_ROM,
    REMU_SYMBOL_ALIAS
} RemuSymbolKind;

typedef struct {
    RemuSymbolKind kind;
    char *name;
    u32 value;
    u16 bank;
} RemuSymbol;

typedef struct {
    RemuSymbol *symbols;
    size_t symbol_count;
    size_t symbol_capacity;
    char *passthrough;
    size_t passthrough_len;
} RemuDebug;

struct CPC;
struct Mem;

void remu_debug_clear(RemuDebug *debug);
int remu_parse_chunk(struct CPC *cpc, const u8 *data, size_t len);
char *remu_build_chunk(const struct CPC *cpc, size_t *out_len);

const RemuSymbol *remu_symbol_lookup(const RemuDebug *debug,
                                     const struct Mem *mem, u16 addr,
                                     u16 max_offset);
const RemuSymbol *remu_symbol_lookup_name(const RemuDebug *debug,
                                          const char *name);
void remu_symbol_format(const RemuDebug *debug, const struct Mem *mem,
                        u16 addr, char *out, size_t out_sz);

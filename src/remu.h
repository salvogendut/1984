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

typedef enum {
    REMU_COMMENT_RAM,
    REMU_COMMENT_ROM
} RemuCommentKind;

typedef struct {
    RemuCommentKind kind;
    u16 address;
    u16 bank;
    char *text;
} RemuComment;

typedef struct {
    u8 id[4];
    u8 *data;
    size_t length;
} RemuOpaqueChunk;

typedef struct {
    RemuSymbol *symbols;
    size_t symbol_count;
    size_t symbol_capacity;
    RemuComment *comments;
    size_t comment_count;
    size_t comment_capacity;
    RemuOpaqueChunk *opaque_chunks;
    size_t opaque_chunk_count;
    size_t opaque_chunk_capacity;
    char *passthrough;
    size_t passthrough_len;
} RemuDebug;

struct CPC;
struct Mem;

void remu_debug_clear(RemuDebug *debug);
int remu_parse_chunk(struct CPC *cpc, const u8 *data, size_t len);
bool remu_preserve_opaque_chunk(RemuDebug *debug, const u8 id[4],
                                const u8 *data, size_t len);
char *remu_build_chunk(const struct CPC *cpc, size_t *out_len);

const RemuSymbol *remu_symbol_lookup(const RemuDebug *debug,
                                     const struct Mem *mem, u16 addr,
                                     u16 max_offset);
const RemuSymbol *remu_symbol_lookup_name(const RemuDebug *debug,
                                          const char *name);
void remu_symbol_format(const RemuDebug *debug, const struct Mem *mem,
                        u16 addr, char *out, size_t out_sz);
const RemuComment *remu_comment_lookup(const RemuDebug *debug,
                                       const struct Mem *mem, u16 addr);
void remu_comment_format(const RemuDebug *debug, const struct Mem *mem,
                         u16 addr, char *out, size_t out_sz);

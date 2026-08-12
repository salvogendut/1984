#define _POSIX_C_SOURCE 200809L

#include "remu.h"
#include "cpc.h"
#include "mem.h"
#include "symbols.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool failed;
    const CPC *cpc;
} RemuBuilder;

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    return s;
}

static bool parse_number(const char *text, u32 *value) {
    while (isspace((unsigned char)*text)) text++;
    int base = 10;
    if (*text == '&' || *text == '#') { base = 16; text++; }
    else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        text += 2;
    }
    if (!*text) return false;
    char *end = NULL;
    unsigned long parsed = strtoul(text, &end, base);
    while (end && isspace((unsigned char)*end)) end++;
    if (!end || *end || parsed > 0xFFFFFFFFul) return false;
    *value = (u32)parsed;
    return true;
}

static bool append_bytes(char **data, size_t *len, const char *src, size_t n) {
    char *grown = realloc(*data, *len + n + 1);
    if (!grown) return false;
    memcpy(grown + *len, src, n);
    *len += n;
    grown[*len] = '\0';
    *data = grown;
    return true;
}

static bool preserve_record(RemuDebug *debug, const char *record) {
    size_t n = strlen(record);
    return append_bytes(&debug->passthrough, &debug->passthrough_len, record, n) &&
           append_bytes(&debug->passthrough, &debug->passthrough_len, ";", 1);
}

static bool add_symbol(RemuDebug *debug, RemuSymbolKind kind,
                       const char *name, u32 value, u16 bank) {
    if (!name || !*name) return false;
    if (debug->symbol_count == debug->symbol_capacity) {
        size_t cap = debug->symbol_capacity ? debug->symbol_capacity * 2 : 64;
        RemuSymbol *grown = realloc(debug->symbols, cap * sizeof(*grown));
        if (!grown) return false;
        debug->symbols = grown;
        debug->symbol_capacity = cap;
    }
    RemuSymbol *symbol = &debug->symbols[debug->symbol_count++];
    symbol->kind = kind;
    symbol->name = strdup(name);
    symbol->value = value;
    symbol->bank = bank;
    if (!symbol->name) {
        debug->symbol_count--;
        return false;
    }
    return true;
}

void remu_debug_clear(RemuDebug *debug) {
    if (!debug) return;
    for (size_t i = 0; i < debug->symbol_count; i++)
        free(debug->symbols[i].name);
    free(debug->symbols);
    free(debug->passthrough);
    memset(debug, 0, sizeof(*debug));
}

static bool parse_break(CPC *cpc, char *args, CpcBreakpointKind kind) {
    char addr_text[64], bank_text[64], extra;
    if (sscanf(args, "%63s %63s %c", addr_text, bank_text, &extra) != 2)
        return false;
    u32 addr, bank;
    if (!parse_number(addr_text, &addr) || !parse_number(bank_text, &bank) ||
            addr > 0xFFFF || bank > 0xFFFF)
        return false;
    if (cpc_breakpoint_add(cpc, (u16)addr, kind, (u16)bank,
                           CPC_BP_SOURCE_SNAPSHOT) < 0) {
        fprintf(stderr, "snapshot: REMU breakpoint limit reached; ignoring %04X\n",
                (unsigned)addr);
    }
    return true;
}

static bool parse_symbol_record(RemuDebug *debug, char *args,
                                RemuSymbolKind kind) {
    char name[256], value_text[64], bank_text[64], extra;
    if (kind == REMU_SYMBOL_ALIAS) {
        if (sscanf(args, "%255s %63s %c", name, value_text, &extra) != 2)
            return false;
        u32 value;
        return parse_number(value_text, &value) &&
               add_symbol(debug, kind, name, value, 0);
    }
    if (sscanf(args, "%255s %63s %63s %c", name, value_text, bank_text,
               &extra) != 3)
        return false;
    u32 value, bank;
    return parse_number(value_text, &value) && value <= 0xFFFF &&
           parse_number(bank_text, &bank) && bank <= 0xFFFF &&
           add_symbol(debug, kind, name, value, (u16)bank);
}

static bool parse_ace_break(CPC *cpc, char *record) {
    char *save = NULL;
    char *token = strtok_r(record, ",", &save);
    if (!token) return false;
    token = trim(token);
    if (strncasecmp(token, "acebreak", 8) != 0) return false;
    char *type = trim(token + 8);
    char *access = strtok_r(NULL, ",", &save);
    char *run_mode = strtok_r(NULL, ",", &save);
    if (!access || !run_mode || strcasecmp(type, "EXEC") != 0 ||
            strcasecmp(trim(run_mode), "STOP") != 0)
        return false;

    u32 addr = 0, mask = 0xFFFF, size = 1;
    bool have_addr = false;
    for (token = strtok_r(NULL, ",", &save); token;
         token = strtok_r(NULL, ",", &save)) {
        token = trim(token);
        char *equals = strchr(token, '=');
        if (!equals) continue;
        *equals++ = '\0';
        char *key = trim(token);
        char *value = trim(equals);
        if (strcasecmp(key, "condition") == 0 && *value)
            return false;
        if (strcasecmp(key, "step") == 0 && *value && strcmp(value, "0") != 0)
            return false;
        if (strcasecmp(key, "addr") == 0) {
            if (!parse_number(value, &addr)) return false;
            have_addr = true;
        } else if (strcasecmp(key, "mask") == 0) {
            if (!parse_number(value, &mask)) return false;
        } else if (strcasecmp(key, "size") == 0) {
            if (!parse_number(value, &size)) return false;
        }
    }
    if (!have_addr || addr > 0xFFFF || mask != 0xFFFF || size != 1)
        return false;
    if (cpc_breakpoint_add(cpc, (u16)addr, CPC_BP_ANY, 0,
                           CPC_BP_SOURCE_SNAPSHOT) < 0)
        fprintf(stderr, "snapshot: REMU breakpoint limit reached; ignoring %04X\n",
                (unsigned)addr);
    return true;
}

static bool parse_record(CPC *cpc, char *record) {
    char *text = trim(record);
    if (!*text) return true;
    if (strncasecmp(text, "romlabel ", 9) == 0)
        return parse_symbol_record(&cpc->remu_debug, trim(text + 9),
                                   REMU_SYMBOL_ROM);
    if (strncasecmp(text, "label ", 6) == 0)
        return parse_symbol_record(&cpc->remu_debug, trim(text + 6),
                                   REMU_SYMBOL_RAM);
    if (strncasecmp(text, "alias ", 6) == 0)
        return parse_symbol_record(&cpc->remu_debug, trim(text + 6),
                                   REMU_SYMBOL_ALIAS);
    if (strncasecmp(text, "rombrk ", 7) == 0)
        return parse_break(cpc, trim(text + 7), CPC_BP_ROM);
    if (strncasecmp(text, "brk ", 4) == 0)
        return parse_break(cpc, trim(text + 4), CPC_BP_RAM);
    if (strncasecmp(text, "acebreak ", 9) == 0) {
        char *copy = strdup(text);
        if (!copy) return false;
        bool parsed = parse_ace_break(cpc, copy);
        free(copy);
        return parsed;
    }
    return false;
}

int remu_parse_chunk(CPC *cpc, const u8 *data, size_t len) {
    if (!cpc || (!data && len)) return -1;
    char *payload = malloc(len + 1);
    if (!payload) return -1;
    memcpy(payload, data, len);
    payload[len] = '\0';

    int parsed = 0;
    char *record = payload;
    for (size_t i = 0; i <= len; i++) {
        if (i != len && payload[i] != ';') continue;
        payload[i] = '\0';
        char *text = trim(record);
        if (*text) {
            if (parse_record(cpc, text)) parsed++;
            else if (!preserve_record(&cpc->remu_debug, text)) {
                free(payload);
                return -1;
            }
        }
        record = payload + i + 1;
    }
    free(payload);
    return parsed;
}

static bool builder_reserve(RemuBuilder *builder, size_t extra) {
    if (builder->len + extra + 1 <= builder->cap) return true;
    size_t cap = builder->cap ? builder->cap : 1024;
    while (cap < builder->len + extra + 1) cap *= 2;
    char *grown = realloc(builder->data, cap);
    if (!grown) return false;
    builder->data = grown;
    builder->cap = cap;
    return true;
}

static bool builder_printf(RemuBuilder *builder, const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    va_list copy;
    va_copy(copy, ap);
    int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0 || !builder_reserve(builder, (size_t)needed)) {
        va_end(ap);
        builder->failed = true;
        return false;
    }
    vsnprintf(builder->data + builder->len, builder->cap - builder->len,
              format, ap);
    va_end(ap);
    builder->len += (size_t)needed;
    return true;
}

static void safe_name(const char *input, char *output, size_t size) {
    size_t used = 0;
    if (!size) return;
    for (; *input && used + 1 < size; input++) {
        unsigned char ch = (unsigned char)*input;
        output[used++] = (isalnum(ch) || ch == '_' || ch == '.' || ch == '$')
                       ? (char)ch : '_';
    }
    output[used] = '\0';
}

static void export_symbol(const Symbol *symbol, int mmr_match, void *userdata) {
    RemuBuilder *builder = userdata;
    u8 ram_bank = mmr_match >= 0 ? (u8)mmr_match : builder->cpc->mem.ram_bank;
    u32 off = mem_ram_offset_for_config(ram_bank, symbol->addr);
    if (off >= (u32)builder->cpc->mem.ram_size) return;
    char name[256];
    safe_name(symbol->name, name, sizeof(name));
    if (*name)
        builder_printf(builder, "label %s %u %u;", name,
                       (unsigned)symbol->addr, (unsigned)(off >> 14));
}

char *remu_build_chunk(const CPC *cpc, size_t *out_len) {
    if (out_len) *out_len = 0;
    if (!cpc) return NULL;
    RemuBuilder builder = { .cpc = cpc };

    for (size_t i = 0; i < cpc->remu_debug.symbol_count; i++) {
        const RemuSymbol *symbol = &cpc->remu_debug.symbols[i];
        char name[256];
        safe_name(symbol->name, name, sizeof(name));
        if (!*name) continue;
        if (symbol->kind == REMU_SYMBOL_ALIAS)
            builder_printf(&builder, "alias %s %u;", name,
                           (unsigned)symbol->value);
        else
            builder_printf(&builder, "%s %s %u %u;",
                           symbol->kind == REMU_SYMBOL_ROM ? "romlabel" : "label",
                           name, (unsigned)symbol->value, (unsigned)symbol->bank);
    }
    symbols_visit(export_symbol, &builder);

    for (int i = 0; i < CPC_MAX_BREAKPOINTS; i++) {
        if (!cpc->bp_enabled[i] ||
                cpc->bp_source[i] == CPC_BP_SOURCE_TEMPORARY)
            continue;
        if (cpc->bp_kind[i] == CPC_BP_RAM)
            builder_printf(&builder, "brk %u %u;",
                           (unsigned)cpc->breakpoints[i],
                           (unsigned)cpc->bp_bank[i]);
        else if (cpc->bp_kind[i] == CPC_BP_ROM)
            builder_printf(&builder, "rombrk %u %u;",
                           (unsigned)cpc->breakpoints[i],
                           (unsigned)cpc->bp_bank[i]);
        else
            builder_printf(&builder,
                "acebreak EXEC,RW,STOP,addr=%u,mask=65535,size=1,value=0,valmask=0,name=1984;",
                (unsigned)cpc->breakpoints[i]);
    }
    if (cpc->remu_debug.passthrough_len) {
        if (!builder_reserve(&builder, cpc->remu_debug.passthrough_len)) {
            free(builder.data);
            if (out_len) *out_len = (size_t)-1;
            return NULL;
        }
        memcpy(builder.data + builder.len, cpc->remu_debug.passthrough,
               cpc->remu_debug.passthrough_len);
        builder.len += cpc->remu_debug.passthrough_len;
        builder.data[builder.len] = '\0';
    }
    if (builder.failed) {
        free(builder.data);
        if (out_len) *out_len = (size_t)-1;
        return NULL;
    }
    if (!builder.len) {
        free(builder.data);
        return NULL;
    }
    if (out_len) *out_len = builder.len;
    return builder.data;
}

const RemuSymbol *remu_symbol_lookup(const RemuDebug *debug, const Mem *mem,
                                     u16 addr, u16 max_offset) {
    if (!debug || !mem) return NULL;
    if (!max_offset) max_offset = 0x100;
    int ram_bank = mem_visible_ram_bank(mem, addr);
    int rom_bank = mem_visible_rom_bank(mem, addr);
    const RemuSymbol *best = NULL;
    u16 best_offset = 0xFFFF;
    for (size_t i = 0; i < debug->symbol_count; i++) {
        const RemuSymbol *symbol = &debug->symbols[i];
        if (symbol->kind == REMU_SYMBOL_ALIAS || symbol->value > addr) continue;
        if (symbol->kind == REMU_SYMBOL_RAM && ram_bank != symbol->bank) continue;
        if (symbol->kind == REMU_SYMBOL_ROM && rom_bank != symbol->bank) continue;
        u16 offset = (u16)(addr - symbol->value);
        if (offset <= max_offset && offset < best_offset) {
            best = symbol;
            best_offset = offset;
        }
    }
    return best;
}

const RemuSymbol *remu_symbol_lookup_name(const RemuDebug *debug,
                                          const char *name) {
    if (!debug || !name) return NULL;
    for (size_t i = 0; i < debug->symbol_count; i++)
        if (strcmp(debug->symbols[i].name, name) == 0)
            return &debug->symbols[i];
    return NULL;
}

void remu_symbol_format(const RemuDebug *debug, const Mem *mem, u16 addr,
                        char *out, size_t out_sz) {
    if (!out_sz) return;
    out[0] = '\0';
    const RemuSymbol *symbol = remu_symbol_lookup(debug, mem, addr, 0);
    if (!symbol) return;
    u16 offset = (u16)(addr - symbol->value);
    if (offset)
        snprintf(out, out_sz, "%s+0x%X", symbol->name, (unsigned)offset);
    else
        snprintf(out, out_sz, "%s", symbol->name);
}

/* symbols.c — see symbols.h.
 *
 * Storage: each loaded .map becomes a `SymbolMap` (its own sorted
 * Symbol[] plus an mmr_match tag). Maps are held in a small linked
 * list — load order is rare and N stays single-digit, so the obvious
 * vector-of-pointers layout is overkill.
 *
 * Lookups walk every map whose tag matches the live ram_bank, picking
 * the symbol with the largest addr <= the query. With ~10K symbols
 * per map and a binary search, a lookup is ~30 comparisons — still
 * cheap enough to do once per disassembled line.
 */
#define _POSIX_C_SOURCE 200809L   /* strdup, ssize_t */

#include "symbols.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>            /* ssize_t */

typedef struct SymbolMap {
    char     *path;
    int       mmr_match;     /* SYMBOLS_ANY_MMR or 0..0xFF */
    Symbol   *syms;          /* sorted ascending by addr */
    size_t    n;
    struct SymbolMap *next;
} SymbolMap;

static SymbolMap *g_maps = NULL;

bool symbols_any_loaded(void) { return g_maps != NULL; }

/* ---- SDCC .map parser --------------------------------------------------
 *
 * SDCC (asxxxx linker) emits lines like:
 *
 *      0000ABCD  _kern_main                          mainmodule
 *
 * Pattern: leading whitespace, exactly 8 hex digits, two-or-more spaces,
 * the identifier, then optional whitespace + module name.
 *
 * Lines we skip:
 *   - Header lines ("Hexadecimal", "Area", "------" etc.)
 *   - Empty lines
 *   - Internal section markers: names starting with `l__` or `s__`
 *   - Special dot symbols: names starting with `.`
 *
 * We deliberately don't try to interpret segment groupings — the goal is
 * a flat addr → name map. Segment metadata can come later via a richer
 * loader if needed.
 */

/* Trim trailing whitespace in place; returns input pointer. */
static char *rtrim(char *s) {
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
    return s;
}

/* Try to parse one SDCC symbol line. On success, fills *out_addr, *out_name
 * (pointing into a freshly-strdup'd buffer that caller owns), *out_module
 * (also strdup, or NULL). Returns true on success. */
static bool parse_sdcc_line(const char *line, u32 *out_addr,
                            char **out_name, char **out_module)
{
    /* Skip leading spaces. */
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') return false;

    /* Expect 8 hex digits. */
    u32 addr = 0;
    int hex_count = 0;
    while (hex_count < 8 && isxdigit((unsigned char)line[hex_count])) {
        char c = line[hex_count];
        u8 v = (c >= '0' && c <= '9') ? (u8)(c - '0')
             : (c >= 'A' && c <= 'F') ? (u8)(10 + c - 'A')
             : (u8)(10 + c - 'a');
        addr = (addr << 4) | v;
        hex_count++;
    }
    if (hex_count != 8) return false;
    if (line[hex_count] != ' ' && line[hex_count] != '\t') return false;

    /* Skip spaces between addr and name. */
    line += hex_count;
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') return false;

    /* Extract name token. Identifier chars: alnum, _, $, . */
    const char *name_start = line;
    while (*line && !isspace((unsigned char)*line)) line++;
    size_t name_len = (size_t)(line - name_start);
    if (name_len == 0) return false;

    /* Filter internal asxxxx markers. */
    if (name_len >= 3 && (memcmp(name_start, "l__", 3) == 0
                       || memcmp(name_start, "s__", 3) == 0)) return false;
    if (name_start[0] == '.') return false;
    /* Skip the special "Module" header row when it appears. */
    if (name_len == 6 && memcmp(name_start, "Global", 6) == 0) return false;

    char *name = (char *)malloc(name_len + 1);
    if (!name) return false;
    memcpy(name, name_start, name_len);
    name[name_len] = '\0';

    /* Optional module name follows. */
    while (*line == ' ' || *line == '\t') line++;
    char *module = NULL;
    if (*line) {
        const char *mod_start = line;
        while (*line && !isspace((unsigned char)*line)) line++;
        size_t mod_len = (size_t)(line - mod_start);
        if (mod_len > 0) {
            module = (char *)malloc(mod_len + 1);
            if (module) {
                memcpy(module, mod_start, mod_len);
                module[mod_len] = '\0';
            }
        }
    }

    *out_addr   = addr;
    *out_name   = name;
    *out_module = module;
    return true;
}

/* Comparator for qsort and bsearch alike. */
static int cmp_sym(const void *a, const void *b) {
    const Symbol *sa = a, *sb = b;
    if (sa->addr < sb->addr) return -1;
    if (sa->addr > sb->addr) return  1;
    return 0;
}

/* Load an SDCC .map file at `path` into a fresh SymbolMap (returned, or
 * NULL on failure). Caller takes ownership. */
static SymbolMap *load_sdcc_map(const char *path, int mmr_match) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "symbols: cannot open '%s'\n", path);
        return NULL;
    }

    /* Two-pass: count, then allocate, then fill. Simpler than a growing
     * vector and avoids realloc churn on big maps (FUZIX kernel = ~5K
     * lines, ~1500 real symbols). */
    size_t cap = 1024;
    Symbol *syms = (Symbol *)malloc(cap * sizeof(*syms));
    size_t n = 0;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        u32 addr; char *name = NULL, *module = NULL;
        if (!parse_sdcc_line(rtrim(line), &addr, &name, &module)) continue;
        if (addr > 0xFFFF) {
            /* SDCC sometimes emits 32-bit absolute values for off-bank
             * data. Keep only the low 16 bits — what the CPU actually
             * sees in any given MMR. */
            addr &= 0xFFFF;
        }
        if (n == cap) {
            cap *= 2;
            Symbol *grown = (Symbol *)realloc(syms, cap * sizeof(*syms));
            if (!grown) { free(name); free(module); break; }
            syms = grown;
        }
        syms[n].addr   = (u16)addr;
        syms[n].name   = name;
        syms[n].module = module;
        n++;
    }
    fclose(f);

    if (n == 0) {
        fprintf(stderr, "symbols: '%s' yielded no usable lines (not an SDCC .map?)\n", path);
        free(syms);
        return NULL;
    }

    qsort(syms, n, sizeof(*syms), cmp_sym);

    SymbolMap *m = (SymbolMap *)calloc(1, sizeof(*m));
    m->path      = strdup(path);
    m->mmr_match = mmr_match;
    m->syms      = syms;
    m->n         = n;
    return m;
}

int symbols_load(const char *path, int mmr_match) {
    /* For now only SDCC is supported. Auto-detection placeholder: read
     * first line, look for the asxxxx linker signature. SCC support will
     * land in a follow-up commit. */
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "symbols: cannot open '%s'\n", path);
        return -1;
    }
    char first[256];
    if (!fgets(first, sizeof(first), f)) {
        fclose(f);
        fprintf(stderr, "symbols: '%s' is empty\n", path);
        return -1;
    }
    fclose(f);

    /* asxxxx-style header looks like:
     *   "ASxxxx Linker V03.00 + NoICE + sdld,  page 1." */
    bool looks_sdcc = (strstr(first, "ASxxxx") != NULL)
                   || (strstr(first, "Linker")  != NULL);
    if (!looks_sdcc) {
        fprintf(stderr, "symbols: '%s' isn't a recognised SDCC .map "
                        "(SCC support is a follow-up)\n", path);
        return -1;
    }

    SymbolMap *m = load_sdcc_map(path, mmr_match);
    if (!m) return -1;

    /* Append to tail so load order is preserved (later loads can shadow
     * earlier ones if the same addr+MMR appears twice — last wins). */
    if (!g_maps) {
        g_maps = m;
    } else {
        SymbolMap *t = g_maps;
        while (t->next) t = t->next;
        t->next = m;
    }
    if (mmr_match == SYMBOLS_ANY_MMR)
        fprintf(stderr, "symbols: loaded %zu syms from '%s'\n", m->n, path);
    else
        fprintf(stderr, "symbols: loaded %zu syms from '%s' (MMR=0x%02X)\n",
                m->n, path, (unsigned)mmr_match);
    return 0;
}

/* Binary search for the largest entry with addr <= target. Returns -1 if
 * no such entry exists (i.e. target is below all addresses). */
static ssize_t bsearch_floor(const Symbol *a, size_t n, u16 target) {
    if (n == 0) return -1;
    ssize_t lo = 0, hi = (ssize_t)n - 1, best = -1;
    while (lo <= hi) {
        ssize_t mid = (lo + hi) / 2;
        if (a[mid].addr <= target) { best = mid; lo = mid + 1; }
        else                       { hi = mid - 1; }
    }
    return best;
}

const Symbol *symbols_lookup(u16 addr, u8 ram_bank, u16 max_offset) {
    if (!g_maps) return NULL;
    if (max_offset == 0) max_offset = 0x100;

    const Symbol *best = NULL;
    u16 best_off = 0xFFFF;
    bool best_mmr_match = false;

    /* Two-tier preference: a map whose mmr_match equals ram_bank beats a
     * SYMBOLS_ANY_MMR map, which beats a map tagged for a *different*
     * bank. The last tier matters in practice — users routinely run
     * `--symbols=01:fuzix.map` and then disassemble before the bank
     * register has been set, expecting to still see annotations. */
    for (SymbolMap *m = g_maps; m; m = m->next) {
        bool exact_mmr = (m->mmr_match == (int)ram_bank);
        bool any_mmr   = (m->mmr_match == SYMBOLS_ANY_MMR);
        ssize_t i = bsearch_floor(m->syms, m->n, addr);
        if (i < 0) continue;
        u16 off = (u16)(addr - m->syms[i].addr);
        if (off > max_offset) continue;

        bool take;
        if (best == NULL) {
            take = true;
        } else if (exact_mmr && !best_mmr_match) {
            take = true;  /* upgrade to a bank-exact match */
        } else if (!exact_mmr && best_mmr_match) {
            take = false; /* keep the bank-exact match we already have */
        } else {
            /* Same tier: closer offset wins; ties go to non-ANY (more specific). */
            take = (off < best_off)
                || (off == best_off && m->mmr_match != SYMBOLS_ANY_MMR && !any_mmr);
        }
        if (take) {
            best = &m->syms[i];
            best_off = off;
            best_mmr_match = exact_mmr;
        }
    }
    return best;
}

const Symbol *symbols_lookup_name(const char *name) {
    if (!g_maps || !name) return NULL;
    for (SymbolMap *m = g_maps; m; m = m->next) {
        for (size_t i = 0; i < m->n; i++)
            if (strcmp(m->syms[i].name, name) == 0)
                return &m->syms[i];
    }
    return NULL;
}

void symbols_format(u16 addr, u8 ram_bank, char *out, size_t out_sz) {
    if (out_sz == 0) return;
    out[0] = '\0';
    if (!g_maps) return;

    const Symbol *s = symbols_lookup(addr, ram_bank, 0);
    if (!s) return;

    u16 off = (u16)(addr - s->addr);
    if (off == 0) snprintf(out, out_sz, "%s", s->name);
    else          snprintf(out, out_sz, "%s+0x%X", s->name, (unsigned)off);
}

void symbols_shutdown(void) {
    SymbolMap *m = g_maps;
    while (m) {
        SymbolMap *next = m->next;
        for (size_t i = 0; i < m->n; i++) {
            free(m->syms[i].name);
            free(m->syms[i].module);
        }
        free(m->syms);
        free(m->path);
        free(m);
        m = next;
    }
    g_maps = NULL;
}

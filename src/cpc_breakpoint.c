#include "cpc.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BREAKPOINT_INITIAL_CAPACITY 16

static void rebuild_address_map(CpcBreakpointManager *manager) {
    memset(manager->address_map, 0, sizeof(manager->address_map));
    for (size_t i = 0; i < manager->count; i++) {
        const CpcBreakpoint *breakpoint = &manager->items[i];
        if (breakpoint->armed)
            manager->address_map[breakpoint->address >> 3] |=
                (u8)(1u << (breakpoint->address & 7));
    }
}

static CpcBreakpoint *find_breakpoint(CpcBreakpointManager *manager,
                                      CpcBreakpointId id) {
    if (id == CPC_BREAKPOINT_INVALID_ID) return NULL;
    for (size_t i = 0; i < manager->count; i++)
        if (manager->items[i].id == id)
            return &manager->items[i];
    return NULL;
}

static bool reserve_breakpoints(CpcBreakpointManager *manager, size_t needed) {
    if (needed <= manager->capacity) return true;
    size_t capacity = manager->capacity ? manager->capacity * 2
                                        : BREAKPOINT_INITIAL_CAPACITY;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) return false;
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*manager->items)) return false;
    CpcBreakpoint *items = realloc(manager->items,
                                   capacity * sizeof(*manager->items));
    if (!items) return false;
    manager->items = items;
    manager->capacity = capacity;
    return true;
}

static CpcBreakpointId allocate_id(CpcBreakpointManager *manager) {
    CpcBreakpointId first = manager->next_id;
    do {
        manager->next_id++;
        if (manager->next_id == CPC_BREAKPOINT_INVALID_ID)
            manager->next_id++;
        if (!find_breakpoint(manager, manager->next_id))
            return manager->next_id;
    } while (manager->next_id != first);
    return CPC_BREAKPOINT_INVALID_ID;
}

CpcBreakpointId cpc_breakpoint_add(CPC *cpc, u16 addr,
                                   CpcBreakpointKind kind, u16 bank,
                                   CpcBreakpointSource source) {
    if (!cpc) return CPC_BREAKPOINT_INVALID_ID;
    CpcBreakpointManager *manager = &cpc->breakpoint_manager;

    if (source != CPC_BP_SOURCE_TEMPORARY) {
        for (size_t i = 0; i < manager->count; i++) {
            CpcBreakpoint *breakpoint = &manager->items[i];
            if (breakpoint->source == (u8)source &&
                    breakpoint->address == addr &&
                    breakpoint->kind == (u8)kind &&
                    breakpoint->bank == bank) {
                breakpoint->armed = source != CPC_BP_SOURCE_SNAPSHOT ||
                                    cpc->snapshot_breakpoints;
                rebuild_address_map(manager);
                return breakpoint->id;
            }
        }
    }

    if (!reserve_breakpoints(manager, manager->count + 1))
        return CPC_BREAKPOINT_INVALID_ID;
    CpcBreakpointId id = allocate_id(manager);
    if (id == CPC_BREAKPOINT_INVALID_ID)
        return CPC_BREAKPOINT_INVALID_ID;

    manager->items[manager->count++] = (CpcBreakpoint) {
        .id = id,
        .address = addr,
        .bank = bank,
        .kind = (u8)kind,
        .source = (u8)source,
        .armed = source != CPC_BP_SOURCE_SNAPSHOT || cpc->snapshot_breakpoints,
    };
    if (manager->items[manager->count - 1].armed)
        manager->address_map[addr >> 3] |= (u8)(1u << (addr & 7));
    return id;
}

void cpc_breakpoint_clear(CPC *cpc, CpcBreakpointId id) {
    if (!cpc || id == CPC_BREAKPOINT_INVALID_ID) return;
    CpcBreakpointManager *manager = &cpc->breakpoint_manager;
    for (size_t i = 0; i < manager->count; i++) {
        if (manager->items[i].id != id) continue;
        if (i + 1 < manager->count)
            memmove(&manager->items[i], &manager->items[i + 1],
                    (manager->count - i - 1) * sizeof(*manager->items));
        manager->count--;
        rebuild_address_map(manager);
        return;
    }
}

void cpc_breakpoint_clear_source(CPC *cpc, CpcBreakpointSource source) {
    if (!cpc) return;
    CpcBreakpointManager *manager = &cpc->breakpoint_manager;
    size_t output = 0;
    for (size_t i = 0; i < manager->count; i++) {
        if (manager->items[i].source == (u8)source) continue;
        if (output != i) manager->items[output] = manager->items[i];
        output++;
    }
    manager->count = output;
    rebuild_address_map(manager);
}

void cpc_breakpoint_set_armed(CPC *cpc, CpcBreakpointId id, bool armed) {
    if (!cpc) return;
    CpcBreakpoint *breakpoint = find_breakpoint(&cpc->breakpoint_manager, id);
    if (!breakpoint || breakpoint->armed == armed) return;
    breakpoint->armed = armed;
    rebuild_address_map(&cpc->breakpoint_manager);
}

void cpc_set_snapshot_breakpoints(CPC *cpc, bool enabled) {
    if (!cpc) return;
    cpc->snapshot_breakpoints = enabled;
    CpcBreakpointManager *manager = &cpc->breakpoint_manager;
    for (size_t i = 0; i < manager->count; i++)
        if (manager->items[i].source == CPC_BP_SOURCE_SNAPSHOT)
            manager->items[i].armed = enabled;
    rebuild_address_map(manager);
}

size_t cpc_breakpoint_count(const CPC *cpc) {
    return cpc ? cpc->breakpoint_manager.count : 0;
}

const CpcBreakpoint *cpc_breakpoint_at(const CPC *cpc, size_t index) {
    if (!cpc || index >= cpc->breakpoint_manager.count) return NULL;
    return &cpc->breakpoint_manager.items[index];
}

const CpcBreakpoint *cpc_breakpoint_get(const CPC *cpc, CpcBreakpointId id) {
    if (!cpc || id == CPC_BREAKPOINT_INVALID_ID) return NULL;
    const CpcBreakpointManager *manager = &cpc->breakpoint_manager;
    for (size_t i = 0; i < manager->count; i++)
        if (manager->items[i].id == id)
            return &manager->items[i];
    return NULL;
}

static bool breakpoint_matches(const CPC *cpc,
                               const CpcBreakpoint *breakpoint, u16 addr) {
    if (!breakpoint->armed || breakpoint->address != addr) return false;
    switch ((CpcBreakpointKind)breakpoint->kind) {
    case CPC_BP_RAM:
        return mem_visible_ram_bank(&cpc->mem, addr) == breakpoint->bank;
    case CPC_BP_ROM:
        return mem_visible_rom_bank(&cpc->mem, addr) == breakpoint->bank;
    default:
        return true;
    }
}

CpcBreakpointId cpc_breakpoint_match(const CPC *cpc, u16 addr,
                                     CpcBreakpointId exclude_id) {
    if (!cpc) return CPC_BREAKPOINT_INVALID_ID;
    const CpcBreakpointManager *manager = &cpc->breakpoint_manager;
    if (!(manager->address_map[addr >> 3] & (u8)(1u << (addr & 7))))
        return CPC_BREAKPOINT_INVALID_ID;
    for (size_t i = 0; i < manager->count; i++) {
        const CpcBreakpoint *breakpoint = &manager->items[i];
        if (breakpoint->id != exclude_id &&
                breakpoint_matches(cpc, breakpoint, addr))
            return breakpoint->id;
    }
    return CPC_BREAKPOINT_INVALID_ID;
}

void cpc_breakpoints_destroy(CPC *cpc) {
    if (!cpc) return;
    free(cpc->breakpoint_manager.items);
    memset(&cpc->breakpoint_manager, 0, sizeof(cpc->breakpoint_manager));
}

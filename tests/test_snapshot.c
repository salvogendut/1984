#define _POSIX_C_SOURCE 200809L

#include "snapshot.h"
#include "remu.h"
#include "symbols.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Snapshot/REMU unit tests link the chip modules, not the SDL host in cpc.c. */
int cpc_breakpoint_add(CPC *cpc, u16 addr, CpcBreakpointKind kind,
                       u16 bank, CpcBreakpointSource source) {
    for (int i = 0; i < CPC_MAX_BREAKPOINTS; i++) {
        if (!cpc->bp_enabled[i]) {
            cpc->breakpoints[i] = addr;
            cpc->bp_kind[i] = kind;
            cpc->bp_bank[i] = bank;
            cpc->bp_source[i] = source;
            cpc->bp_enabled[i] = true;
            return i;
        }
    }
    return -1;
}

void cpc_breakpoint_clear(CPC *cpc, int slot) {
    if (slot >= 0 && slot < CPC_MAX_BREAKPOINTS)
        cpc->bp_enabled[slot] = false;
}

void cpc_breakpoint_clear_source(CPC *cpc, CpcBreakpointSource source) {
    for (int i = 0; i < CPC_MAX_BREAKPOINTS; i++)
        if (cpc->bp_enabled[i] && cpc->bp_source[i] == source)
            cpc->bp_enabled[i] = false;
}

bool cpc_breakpoint_matches(const CPC *cpc, int slot, u16 addr) {
    (void)cpc; (void)slot; (void)addr;
    return false;
}

static void put32(u8 *p, u32 value) {
    p[0] = value & 0xFF;
    p[1] = (value >> 8) & 0xFF;
    p[2] = (value >> 16) & 0xFF;
    p[3] = (value >> 24) & 0xFF;
}

static void write_chunk(FILE *file, const char id[4],
                        const void *payload, size_t length) {
    u8 header[8];
    memcpy(header, id, 4);
    put32(header + 4, (u32)length);
    assert(fwrite(header, 1, sizeof(header), file) == sizeof(header));
    assert(fwrite(payload, 1, length, file) == length);
}

static size_t encode_zero_bank(u8 *output) {
    size_t used = 0;
    size_t remaining = 0x10000;
    while (remaining) {
        u8 count = remaining > 255 ? 255 : (u8)remaining;
        output[used++] = 0xE5;
        output[used++] = count;
        output[used++] = 0;
        remaining -= count;
    }
    return used;
}

static void init_cpc(CPC *cpc) {
    mem_init(&cpc->mem);
    cpc->mem.ram_size = 0x10000;
    ga_init(&cpc->ga);
    crtc_init(&cpc->crtc);
    ppi_init(&cpc->ppi);
    psg_init(&cpc->psg);
}

static void test_chunked_snapshot(void) {
    char input[128], output[128], map_path[128];
    snprintf(input, sizeof(input), "/tmp/1984-remu-%ld-in.sna", (long)getpid());
    snprintf(output, sizeof(output), "/tmp/1984-remu-%ld-out.sna", (long)getpid());
    snprintf(map_path, sizeof(map_path), "/tmp/1984-remu-%ld.map", (long)getpid());

    u8 header[256] = {0};
    memcpy(header, "MV - SNA", 8);
    header[0x10] = 3;
    header[0x23] = 0x00;
    header[0x24] = 0x20;
    header[0x6B] = 0; /* RASM supersnapshot: RAM is in MEMx chunks. */
    header[0x6C] = 0;

    u8 *mem0 = malloc(0x10000);
    u8 *compressed = malloc(1024);
    assert(mem0 && compressed);
    for (size_t i = 0; i < 0x10000; i++) mem0[i] = (u8)i;
    size_t compressed_len = encode_zero_bank(compressed);

    const char remu[] =
        "label main 16384 1;"
        "romlabel firmware 0 256;"
        "alias answer 42;"
        "brk 16384 1;"
        "rombrk 16 256;"
        "acebreak EXEC,RW,STOP,addr=8192,mask=65535,size=1,value=0,valmask=0,name=test;"
        "acebreak MEM,RW,STOP,addr=32,mask=65535,size=1,value=0,valmask=0,name=watch;"
        "comz source-note;";

    FILE *file = fopen(input, "wb");
    assert(file);
    assert(fwrite(header, 1, sizeof(header), file) == sizeof(header));
    write_chunk(file, "MEM0", mem0, 0x10000);
    write_chunk(file, "MEM1", compressed, compressed_len);
    write_chunk(file, "REMU", remu, sizeof(remu) - 1);
    fclose(file);

    CPC *cpc = calloc(1, sizeof(*cpc));
    assert(cpc);
    init_cpc(cpc);
    assert(snapshot_load(cpc, input) == 0);
    assert(cpc->mem.ram_size == 0x20000);
    assert(cpc->mem.ram[0x1234] == 0x34);
    assert(cpc->mem.ram[0x10000] == 0);
    assert(cpc->mem.ram[0x1FFFF] == 0);
    assert(cpc->remu_debug.symbol_count == 3);
    assert(cpc->remu_debug.passthrough &&
           strstr(cpc->remu_debug.passthrough, "acebreak MEM") &&
           strstr(cpc->remu_debug.passthrough, "comz source-note;"));
    assert(cpc->bp_enabled[0] && cpc->bp_kind[0] == CPC_BP_RAM &&
           cpc->bp_bank[0] == 1);
    assert(cpc->bp_enabled[1] && cpc->bp_kind[1] == CPC_BP_ROM &&
           cpc->bp_bank[1] == 256);
    assert(cpc->bp_enabled[2] && cpc->bp_kind[2] == CPC_BP_ANY);

    cpc->mem.lower_rom_enabled = false;
    cpc->mem.upper_rom_enabled = false;
    const RemuSymbol *symbol = remu_symbol_lookup(&cpc->remu_debug,
                                                  &cpc->mem, 0x4000, 0);
    assert(symbol && strcmp(symbol->name, "main") == 0);
    assert(remu_symbol_lookup_name(&cpc->remu_debug, "answer")->value == 42);

    file = fopen(map_path, "w");
    assert(file);
    fputs("ASxxxx Linker test map\n00004000  _mapped_entry  module\n", file);
    fclose(file);
    assert(symbols_load(map_path, 4) == 0);
    assert(snapshot_save(cpc, output) == 0);
    file = fopen(output, "rb");
    assert(file);
    assert(fseek(file, 256 + cpc->mem.ram_size, SEEK_SET) == 0);
    u8 chunk_header[8];
    assert(fread(chunk_header, 1, sizeof(chunk_header), file) == sizeof(chunk_header));
    assert(memcmp(chunk_header, "REMU", 4) == 0);
    u32 saved_len = (u32)chunk_header[4] | ((u32)chunk_header[5] << 8) |
                    ((u32)chunk_header[6] << 16) | ((u32)chunk_header[7] << 24);
    char *saved = malloc(saved_len + 1);
    assert(saved);
    assert(fread(saved, 1, saved_len, file) == saved_len);
    saved[saved_len] = '\0';
    assert(strstr(saved, "label main 16384 1;"));
    assert(strstr(saved, "label _mapped_entry 16384 4;"));
    assert(strstr(saved, "brk 16384 1;"));
    assert(strstr(saved, "acebreak MEM"));
    fclose(file);

    free(saved);
    symbols_shutdown();
    remu_debug_clear(&cpc->remu_debug);
    free(cpc);
    free(compressed);
    free(mem0);
    unlink(input);
    unlink(output);
    unlink(map_path);
}

int main(void) {
    test_chunked_snapshot();
    puts("snapshot/REMU tests passed");
    return 0;
}

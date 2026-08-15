#define _POSIX_C_SOURCE 200809L

#include "snapshot.h"
#include "remu.h"
#include "symbols.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static size_t encode_fill(u8 *output, size_t size, u8 value) {
    size_t used = 0;
    size_t remaining = size;
    while (remaining) {
        u8 count = remaining > 255 ? 255 : (u8)remaining;
        output[used++] = 0xE5;
        output[used++] = count;
        output[used++] = value;
        remaining -= count;
    }
    return used;
}

static size_t encode_zero_bank(u8 *output) {
    return encode_fill(output, 0x10000, 0);
}

static char *read_named_chunk(FILE *file, const char wanted[4], u32 *out_len) {
    u8 header[8];
    while (fread(header, 1, sizeof(header), file) == sizeof(header)) {
        u32 length = (u32)header[4] | ((u32)header[5] << 8) |
                     ((u32)header[6] << 16) | ((u32)header[7] << 24);
        if (memcmp(header, wanted, 4) == 0) {
            char *data = malloc((size_t)length + 1);
            assert(data);
            assert(fread(data, 1, length, file) == length);
            data[length] = '\0';
            *out_len = length;
            return data;
        }
        assert(fseek(file, (long)length, SEEK_CUR) == 0);
    }
    return NULL;
}

static void init_cpc(CPC *cpc) {
    cpc->snapshot_breakpoints = true;
    mem_init(&cpc->mem);
    cpc->mem.ram_size = 0x10000;
    ga_init(&cpc->ga);
    crtc_init(&cpc->crtc);
    ppi_init(&cpc->ppi);
    psg_init(&cpc->psg);
}

static void test_extended_memory_chunk(void) {
    char path[128];
    snprintf(path, sizeof(path), "/tmp/1984-mx-%ld.sna", (long)getpid());

    u8 header[256] = {0};
    memcpy(header, "MV - SNA", 8);
    header[0x10] = 3;
    u8 compressed[1024];
    size_t length;

    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(header, 1, sizeof(header), file) == sizeof(header));
    write_chunk(file, "MEM0", compressed, encode_fill(compressed, 0x10000, 0));
    length = encode_fill(compressed, 0x10000, 0xA9);
    write_chunk(file, "MX09", compressed, length);
    fclose(file);

    CPC *cpc = calloc(1, sizeof(*cpc));
    assert(cpc);
    init_cpc(cpc);
    assert(snapshot_load(cpc, path) == 0);
    assert(cpc->mem.ram_size == 10 * 0x10000);
    assert(cpc->mem.ram[9 * 0x10000] == 0xA9);
    assert(cpc->mem.ram[10 * 0x10000 - 1] == 0xA9);

    mem_clear_snapshot_roms(&cpc->mem);
    remu_debug_clear(&cpc->remu_debug);
    cpc_breakpoints_destroy(cpc);
    free(cpc);
    unlink(path);
}

static void test_chunked_snapshot(void) {
    char input[128], output[128], plain[128], map_path[128];
    snprintf(input, sizeof(input), "/tmp/1984-remu-%ld-in.sna", (long)getpid());
    snprintf(output, sizeof(output), "/tmp/1984-remu-%ld-out.sna", (long)getpid());
    snprintf(plain, sizeof(plain), "/tmp/1984-remu-%ld-plain.sna", (long)getpid());
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
    u8 *lower = malloc(ROM_OS_SIZE);
    u8 *upper_fe = malloc(ROM_BASIC_SIZE);
    u8 compressed_rom[256];
    assert(mem0 && compressed && lower && upper_fe);
    for (size_t i = 0; i < 0x10000; i++) mem0[i] = (u8)i;
    for (size_t i = 0; i < ROM_OS_SIZE; i++) lower[i] = (u8)(i * 3 + 1);
    for (size_t i = 0; i < ROM_BASIC_SIZE; i++) upper_fe[i] = (u8)(i * 5 + 7);
    size_t compressed_len = encode_zero_bank(compressed);
    size_t compressed_rom_len = encode_fill(compressed_rom, ROM_BASIC_SIZE, 0x5A);

    const char remu[] =
        "label main 16384 1;"
        "romlabel firmware 0 256;"
        "alias answer 42;"
        "brk 16384 1;"
        "rombrk 16 256;"
        "comz 16384 1 source entry;"
        "romcomz 16 256 firmware entry;"
        "acebreak EXEC,RW,STOP,addr=8192,mask=65535,size=1,value=0,valmask=0,name=test;"
        "acebreak MEM,RW,STOP,addr=32,mask=65535,size=1,value=0,valmask=0,name=watch;"
        "futuretag source-note;";
    const u8 brks[] = { 0x00, 0x40, 0x01, 0x00, 0x00 };

    FILE *file = fopen(input, "wb");
    assert(file);
    assert(fwrite(header, 1, sizeof(header), file) == sizeof(header));
    write_chunk(file, "MEM0", mem0, 0x10000);
    write_chunk(file, "MEM1", compressed, compressed_len);
    write_chunk(file, "LOWR", lower, ROM_OS_SIZE);
    write_chunk(file, "RM05", compressed_rom, compressed_rom_len);
    write_chunk(file, "RMFE", upper_fe, ROM_BASIC_SIZE);
    write_chunk(file, "REMU", remu, sizeof(remu) - 1);
    write_chunk(file, "BRKS", brks, sizeof(brks));
    fclose(file);

    CPC *cpc = calloc(1, sizeof(*cpc));
    assert(cpc);
    init_cpc(cpc);
    assert(snapshot_load(cpc, input) == 0);
    assert(cpc->mem.ram_size == 0x20000);
    assert(cpc->mem.ram[0x1234] == 0x34);
    assert(cpc->mem.ram[0x10000] == 0);
    assert(cpc->mem.ram[0x1FFFF] == 0);
    assert(mem_get_snapshot_rom(&cpc->mem, SNAPSHOT_ROM_COUNT));
    assert(mem_get_snapshot_rom(&cpc->mem, 5));
    assert(mem_get_snapshot_rom(&cpc->mem, 0xFE));
    assert(mem_read(&cpc->mem, 0x0010) == lower[0x10]);
    cpc->mem.upper_rom_select = 5;
    assert(mem_read(&cpc->mem, 0xC123) == 0x5A);
    cpc->mem.upper_rom_select = 0xFE;
    assert(mem_read(&cpc->mem, 0xC321) == upper_fe[0x321]);
    assert(cpc->remu_debug.symbol_count == 3);
    assert(cpc->remu_debug.comment_count == 2);
    assert(cpc->remu_debug.opaque_chunk_count == 1);
    assert(memcmp(cpc->remu_debug.opaque_chunks[0].id, "BRKS", 4) == 0);
    assert(cpc->remu_debug.opaque_chunks[0].length == sizeof(brks));
    assert(memcmp(cpc->remu_debug.opaque_chunks[0].data,
                  brks, sizeof(brks)) == 0);
    assert(cpc->remu_debug.passthrough &&
           strstr(cpc->remu_debug.passthrough, "acebreak MEM") &&
           strstr(cpc->remu_debug.passthrough, "futuretag source-note;"));
    assert(cpc_breakpoint_count(cpc) == 3);
    const CpcBreakpoint *breakpoint = cpc_breakpoint_at(cpc, 0);
    assert(breakpoint && breakpoint->kind == CPC_BP_RAM &&
           breakpoint->bank == 1 && breakpoint->armed);
    breakpoint = cpc_breakpoint_at(cpc, 1);
    assert(breakpoint && breakpoint->kind == CPC_BP_ROM &&
           breakpoint->bank == 256 && breakpoint->armed);
    breakpoint = cpc_breakpoint_at(cpc, 2);
    assert(breakpoint && breakpoint->kind == CPC_BP_ANY && breakpoint->armed);
    cpc_set_snapshot_breakpoints(cpc, false);
    for (size_t i = 0; i < cpc_breakpoint_count(cpc); i++)
        assert(!cpc_breakpoint_at(cpc, i)->armed);
    cpc_set_snapshot_breakpoints(cpc, true);
    for (size_t i = 0; i < cpc_breakpoint_count(cpc); i++)
        assert(cpc_breakpoint_at(cpc, i)->armed);

    const RemuComment *comment = remu_comment_lookup(&cpc->remu_debug,
                                                      &cpc->mem, 0x0010);
    assert(comment && strcmp(comment->text, "firmware entry") == 0);
    cpc->mem.lower_rom_enabled = false;
    cpc->mem.upper_rom_enabled = false;
    const RemuSymbol *symbol = remu_symbol_lookup(&cpc->remu_debug,
                                                  &cpc->mem, 0x4000, 0);
    assert(symbol && strcmp(symbol->name, "main") == 0);
    assert(remu_symbol_lookup_name(&cpc->remu_debug, "answer")->value == 42);
    comment = remu_comment_lookup(&cpc->remu_debug, &cpc->mem, 0x4000);
    assert(comment && strcmp(comment->text, "source entry") == 0);

    file = fopen(map_path, "w");
    assert(file);
    fputs("ASxxxx Linker test map\n00004000  _mapped_entry  module\n", file);
    fclose(file);
    assert(symbols_load(map_path, 4) == 0);
    assert(snapshot_save(cpc, output) == 0);
    file = fopen(output, "rb");
    assert(file);
    assert(fseek(file, 256 + cpc->mem.ram_size, SEEK_SET) == 0);
    u32 saved_len = 0;
    char *saved = read_named_chunk(file, "REMU", &saved_len);
    assert(saved && saved_len > 0);
    assert(strstr(saved, "label main 16384 1;"));
    assert(strstr(saved, "label _mapped_entry 16384 4;"));
    assert(strstr(saved, "brk 16384 1;"));
    assert(strstr(saved, "comz 16384 1 source entry;"));
    assert(strstr(saved, "romcomz 16 256 firmware entry;"));
    assert(strstr(saved, "acebreak MEM"));
    fclose(file);

    file = fopen(output, "rb");
    assert(file);
    assert(fseek(file, 256 + cpc->mem.ram_size, SEEK_SET) == 0);
    u32 opaque_len = 0;
    char *saved_brks = read_named_chunk(file, "BRKS", &opaque_len);
    assert(saved_brks && opaque_len == sizeof(brks));
    assert(memcmp(saved_brks, brks, sizeof(brks)) == 0);
    free(saved_brks);
    fclose(file);

    CPC *roundtrip = calloc(1, sizeof(*roundtrip));
    assert(roundtrip);
    init_cpc(roundtrip);
    assert(snapshot_load(roundtrip, output) == 0);
    assert(memcmp(mem_get_snapshot_rom(&roundtrip->mem, SNAPSHOT_ROM_COUNT),
                  lower, ROM_OS_SIZE) == 0);
    assert(memcmp(mem_get_snapshot_rom(&roundtrip->mem, 0xFE),
                  upper_fe, ROM_BASIC_SIZE) == 0);
    assert(mem_get_snapshot_rom(&roundtrip->mem, 5)[0x123] == 0x5A);
    assert(roundtrip->remu_debug.comment_count == 2);
    assert(roundtrip->remu_debug.opaque_chunk_count == 1);

    /* A conventional SNA must remove supersnapshot ROM overlays and reveal
     * the machine's configured ROMs again. */
    file = fopen(plain, "wb");
    assert(file);
    header[0x6B] = 64;
    assert(fwrite(header, 1, sizeof(header), file) == sizeof(header));
    assert(fwrite(mem0, 1, 0x10000, file) == 0x10000);
    fclose(file);
    assert(snapshot_load(roundtrip, plain) == 0);
    assert(!mem_get_snapshot_rom(&roundtrip->mem, SNAPSHOT_ROM_COUNT));
    assert(!mem_get_snapshot_rom(&roundtrip->mem, 5));
    assert(!mem_get_snapshot_rom(&roundtrip->mem, 0xFE));
    assert(roundtrip->remu_debug.opaque_chunk_count == 0);

    free(saved);
    symbols_shutdown();
    mem_clear_snapshot_roms(&roundtrip->mem);
    remu_debug_clear(&roundtrip->remu_debug);
    cpc_breakpoints_destroy(roundtrip);
    free(roundtrip);
    mem_clear_snapshot_roms(&cpc->mem);
    remu_debug_clear(&cpc->remu_debug);
    cpc_breakpoints_destroy(cpc);
    free(cpc);
    free(compressed);
    free(upper_fe);
    free(lower);
    free(mem0);
    unlink(input);
    unlink(output);
    unlink(plain);
    unlink(map_path);
}

int main(void) {
    test_extended_memory_chunk();
    test_chunked_snapshot();
    puts("snapshot/REMU tests passed");
    return 0;
}

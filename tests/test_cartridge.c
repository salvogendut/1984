#include "../src/cartridge.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void write_le32(FILE *file, uint32_t value)
{
    const u8 bytes[4] = {
        (u8)value, (u8)(value >> 8), (u8)(value >> 16), (u8)(value >> 24)
    };
    assert(fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes));
}

static void write_chunk(FILE *file, const char id[4],
                        const u8 *data, uint32_t size)
{
    assert(fwrite(id, 1, 4, file) == 4);
    write_le32(file, size);
    if (size)
        assert(fwrite(data, 1, size, file) == size);
    if (size & 1u)
        assert(fputc(0, file) != EOF);
}

static FILE *valid_sparse_cpr(void)
{
    FILE *file = tmpfile();
    assert(file);

    const u8 page3[] = { 0x33, 0x34, 0x35 };
    const u8 page0[] = { 0x00, 0x01, 0x02, 0x03 };
    uint32_t riff_size = 4
                       + 8
                       + 8 + sizeof(page3) + 1
                       + 8 + sizeof(page0);

    assert(fwrite("RIFF", 1, 4, file) == 4);
    write_le32(file, riff_size);
    assert(fwrite("AMS!", 1, 4, file) == 4);
    write_chunk(file, "fmt ", NULL, 0);
    write_chunk(file, "cb03", page3, sizeof(page3));
    write_chunk(file, "cb00", page0, sizeof(page0));
    rewind(file);
    return file;
}

static void test_sparse_out_of_order_pages(void)
{
    Cartridge cart;
    FILE *file = valid_sparse_cpr();

    assert(cartridge_load_file(&cart, file) == 0);
    fclose(file);

    assert(cartridge_page_present(&cart, 0));
    assert(cartridge_page_present(&cart, 3));
    assert(!cartridge_page_present(&cart, 1));
    assert(cartridge_page(&cart, 0)[2] == 0x02);
    assert(cartridge_page(&cart, 3)[0] == 0x33);
    assert(cartridge_page(&cart, 3)[3] == 0x00);
    assert(cartridge_page(&cart, 1) == NULL);
}

static void test_invalid_header(void)
{
    Cartridge cart;
    FILE *file = tmpfile();
    assert(file);
    assert(fwrite("not a cartridge", 1, 15, file) == 15);
    rewind(file);
    assert(cartridge_load_file(&cart, file) < 0);
    fclose(file);
}

static void test_truncated_chunk(void)
{
    Cartridge cart;
    FILE *file = tmpfile();
    assert(file);
    assert(fwrite("RIFF", 1, 4, file) == 4);
    write_le32(file, 4 + 8 + 16);
    assert(fwrite("AMS!cb00", 1, 8, file) == 8);
    write_le32(file, 16);
    assert(fputc(0x42, file) != EOF);
    rewind(file);
    assert(cartridge_load_file(&cart, file) < 0);
    fclose(file);
}

int main(void)
{
    test_sparse_out_of_order_pages();
    test_invalid_header();
    test_truncated_chunk();
    puts("cartridge tests passed");
    return 0;
}

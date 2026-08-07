#include "cartridge.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint32_t read_le32(const u8 *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static int chunk_page(const u8 id[4])
{
    if (id[0] != 'c' || id[1] != 'b' ||
        !isdigit((unsigned char)id[2]) ||
        !isdigit((unsigned char)id[3]))
        return -1;

    int page = (id[2] - '0') * 10 + (id[3] - '0');
    return page < CARTRIDGE_PAGE_COUNT ? page : -1;
}

static bool skip_bytes(FILE *file, uint32_t count)
{
    u8 discard[1024];
    while (count) {
        size_t n = count < sizeof(discard) ? count : sizeof(discard);
        if (fread(discard, 1, n, file) != n)
            return false;
        count -= (uint32_t)n;
    }
    return true;
}

void cartridge_init(Cartridge *cart)
{
    memset(cart->page, 0xFF, sizeof(cart->page));
    memset(cart->present, 0, sizeof(cart->present));
}

int cartridge_load_file(Cartridge *cart, FILE *file)
{
    u8 header[12];
    if (!cart || !file)
        return -1;

    cartridge_init(cart);
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "RIFF", 4) != 0 ||
        memcmp(header + 8, "AMS!", 4) != 0)
        return -1;

    uint32_t riff_size = read_le32(header + 4);
    if (riff_size < 4)
        return -1;
    uint32_t remaining = riff_size - 4; /* form type was consumed above */
    bool any_page = false;

    while (remaining) {
        u8 chunk[8];
        if (remaining < sizeof(chunk) ||
            fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk))
            return -1;
        remaining -= sizeof(chunk);

        uint32_t size = read_le32(chunk + 4);
        uint32_t padded = size + (size & 1u);
        if (padded < size || padded > remaining)
            return -1;

        int page = chunk_page(chunk);
        if (page >= 0) {
            uint32_t kept = size < CARTRIDGE_PAGE_SIZE
                          ? size : CARTRIDGE_PAGE_SIZE;
            memset(cart->page[page], 0, CARTRIDGE_PAGE_SIZE);
            if (kept && fread(cart->page[page], 1, kept, file) != kept)
                return -1;
            if (!skip_bytes(file, size - kept))
                return -1;
            cart->present[page] = true;
            any_page = true;
        } else if (!skip_bytes(file, size)) {
            return -1;
        }

        if ((size & 1u) && fgetc(file) == EOF)
            return -1;
        remaining -= padded;
    }

    return any_page ? 0 : -1;
}

int cartridge_load(Cartridge *cart, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Cannot open CPR cartridge: %s\n", path);
        return -1;
    }

    int result = cartridge_load_file(cart, file);
    fclose(file);
    if (result < 0)
        fprintf(stderr, "Invalid CPR cartridge: %s\n", path);
    return result;
}

bool cartridge_page_present(const Cartridge *cart, unsigned page)
{
    return cart && page < CARTRIDGE_PAGE_COUNT && cart->present[page];
}

const u8 *cartridge_page(const Cartridge *cart, unsigned page)
{
    return cartridge_page_present(cart, page) ? cart->page[page] : NULL;
}

#pragma once

#include <stdbool.h>
#include <stdio.h>

#include "types.h"

#define CARTRIDGE_PAGE_COUNT 32
#define CARTRIDGE_PAGE_SIZE  0x4000

typedef struct {
    u8   page[CARTRIDGE_PAGE_COUNT][CARTRIDGE_PAGE_SIZE];
    bool present[CARTRIDGE_PAGE_COUNT];
} Cartridge;

void cartridge_init(Cartridge *cart);
int  cartridge_load(Cartridge *cart, const char *path);
int  cartridge_load_file(Cartridge *cart, FILE *file);

bool      cartridge_page_present(const Cartridge *cart, unsigned page);
const u8 *cartridge_page(const Cartridge *cart, unsigned page);

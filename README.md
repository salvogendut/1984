# 1984 — Amstrad CPC Emulator

A cycle-stepped Amstrad CPC 464/6128 emulator written in C with SDL2.

## Status

Boots to Locomotive BASIC. The BASIC prompt, copyright banner, and keyboard input work. Audio (AY-3-8912 / PSG) and disk loading are not yet implemented.

## Requirements

- GCC (C11)
- SDL2 (`libsdl2-dev` on Debian/Ubuntu)
- CPC ROM images (not included — dump your own or source from the web)

## Build

```bash
make
```

Output binary: `bin/cpc1984`

## ROM files

Place ROM images in the `roms/` directory with these exact names:

| File | Contents |
|------|----------|
| `roms/OS_464.ROM` | CPC 464 OS ROM (16 KB) |
| `roms/BASIC_1.0.ROM` | CPC 464 Locomotive BASIC 1.0 (16 KB) |
| `roms/OS_6128.ROM` | CPC 6128 OS ROM (16 KB) |
| `roms/BASIC_1.1.ROM` | CPC 6128 Locomotive BASIC 1.1 (16 KB) |

## Usage

```bash
# Run as CPC 6128 (default)
./bin/cpc1984

# Run as CPC 464
./bin/cpc1984 4
```

Press **F12** to quit.

## Development

See [DEVELOPMENT.md](DEVELOPMENT.md) for architecture details, timing, video rendering, memory map, and I/O decoding.

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

## Architecture

Each source file maps to one hardware component:

| File | Component |
|------|-----------|
| `src/z80.c` / `z80.h` | Z80 CPU — full instruction set, all prefixes (CB/DD/ED/FD), interrupts IM0/1/2 |
| `src/mem.c` / `mem.h` | Memory map — lower/upper ROM overlay, 6128 RAM banking via Gate Array |
| `src/crtc.c` / `crtc.h` | MC6845 CRTC — horizontal/vertical timing, MA/RA address generation, display enable |
| `src/gate_array.c` / `gate_array.h` | Gate Array — screen mode, ink palette (32 hardware colours), ROM enables, interrupt counter |
| `src/ppi.c` / `ppi.h` | 8255 PPI — keyboard row selection, vsync feedback, PSG control routing |
| `src/psg.c` / `psg.h` | AY-3-8912 PSG register file (audio generation not yet wired to SDL audio) |
| `src/kbd.c` / `kbd.h` | Keyboard matrix — SDL scancode → CPC row/column mapping |
| `src/display.c` / `display.h` | SDL2 display — 768×272 pixel buffer, presented at 768×576 (4:3 aspect) |
| `src/cpc.c` / `cpc.h` | Top-level machine — bus wiring, frame execution, pixel rendering |
| `src/main.c` | Entry point — SDL init, model selection, event loop |

### Timing

The CPU runs at 4 MHz. The CRTC ticks at 1 MHz (one character clock every 4 CPU cycles). Each character clock outputs 16 pixels. A PAL frame is 50 Hz (80,000 CPU cycles). VSYNC triggers `display_present()` which uploads the pixel buffer to the SDL texture.

### Video rendering

The pixel buffer is 768×272. The CRTC's MA and RA registers are used to compute the video RAM address:

```
addr = (MA[13:12] << 14) | (RA[2:0] << 11) | (MA[9:0] << 1)
```

Two bytes are fetched per character clock and decoded according to the Gate Array screen mode:

- **Mode 0** — 4 bpp, 2 pixels/byte, each pixel 4× wide (160×200)
- **Mode 1** — 2 bpp, 4 pixels/byte, each pixel 2× wide (320×200)
- **Mode 2** — 1 bpp, 8 pixels/byte, 1:1 (640×200)

### Memory map

```
0x0000–0x3FFF   OS ROM (lower) or RAM
0x4000–0x7FFF   RAM
0x8000–0xBFFF   RAM
0xC000–0xFFFF   BASIC ROM (upper) or RAM bank (6128)
```

The 6128 has 128 KB RAM. Extra 16 KB pages are selected via Gate Array port 0x7Fxx (function bits 11xxxxxx).

### I/O decoding

| Address bits | Device | Port example |
|---|---|---|
| A15=0, A14=1 | Gate Array | 0x7Fxx |
| A14=0, A8=0 | CRTC select | 0xBCxx |
| A14=0, A8=1 | CRTC write/read | 0xBDxx / 0xBFxx |
| A11=0 | PPI (8255) | 0xF4–0xF7xx |

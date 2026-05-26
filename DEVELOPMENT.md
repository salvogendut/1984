# Development

## Architecture

Each source file maps to one hardware component:

| File | Component |
|------|-----------|
| `src/z80.c` / `z80.h` | Z80 CPU — full documented instruction set plus undocumented IX/IY half-register ops, all prefixes (CB/DD/ED/FD), interrupts IM0/1/2 |
| `src/mem.c` / `mem.h` | Memory map — lower/upper ROM overlay, 6128 RAM banking via Gate Array |
| `src/crtc.c` / `crtc.h` | MC6845 CRTC — horizontal/vertical timing, MA/RA address generation, display enable |
| `src/gate_array.c` / `gate_array.h` | Gate Array — screen mode, ink palette (32 hardware colours), ROM enables, interrupt counter |
| `src/ppi.c` / `ppi.h` | 8255 PPI — keyboard row selection, vsync feedback, PSG control routing |
| `src/psg.c` / `psg.h` | AY-3-8912 PSG register file (audio generation not yet wired to SDL audio) |
| `src/kbd.c` / `kbd.h` | Keyboard matrix — SDL scancode → CPC row/column mapping |
| `src/display.c` / `display.h` | SDL3 display — 768×272 pixel buffer, letterboxed into the window at 4:3 aspect |
| `src/disk.c` / `disk.h` | DSK disk image parser — track/sector layout, AMSDOS directory, read |
| `src/fdc.c` / `fdc.h` | µPD765 FDC — command/exec/result phases, READ DATA, SEEK, SENSE INTERRUPT STATUS |
| `src/cpc.c` / `cpc.h` | Top-level machine — bus wiring, frame execution, pixel rendering, reset |
| `src/config.c` / `config.h` | INI config file — load/save `~/.config/1984/1984.conf`, first-run creation, model defaults |
| `src/overlay.c` / `overlay.h` | SDL3 in-app options overlay — tabbed menu, dirty tracking, save-on-close prompt |
| `src/paste.c` / `paste.h` | Host-to-emulator paste — queues clipboard text and injects keypresses into the CPC matrix |
| `src/main.c` | Entry point — SDL init, event loop, F5/F9/F12/Ctrl+V handling |

## Render pipeline

The frame render is split into two phases to allow the overlay to composite on top of the CPC video output:

1. **`cpc_frame()`** — runs the CPU and CRTC for one PAL frame (80,000 cycles), writing pixels into `display.pixels[]`
2. **`display_upload()`** — uploads the pixel buffer to the SDL texture, clears the renderer, and blits the texture letterboxed into the window
3. **`overlay_render()`** — draws the overlay on top of the renderer (if visible), using `SDL_SetRenderScale` at 1.5× for the bitmap font
4. **`display_flip()`** — calls `SDL_RenderPresent`

## Timing

The CPU runs at 4 MHz. The CRTC ticks at 1 MHz (one character clock every 4 CPU cycles). Each character clock outputs 16 pixels. A PAL frame is 50 Hz (80,000 CPU cycles). VSYNC rising edge triggers `display_upload()`.

## Video rendering

The pixel buffer is 768×272. The CRTC's MA and RA registers are used to compute the video RAM address:

```
addr = (MA[13:12] << 14) | (RA[2:0] << 11) | (MA[9:0] << 1)
```

Two bytes are fetched per character clock and decoded according to the Gate Array screen mode:

- **Mode 0** — 4 bpp, 2 pixels/byte, each pixel 4× wide (160×200)
- **Mode 1** — 2 bpp, 4 pixels/byte, each pixel 2× wide (320×200)
- **Mode 2** — 1 bpp, 8 pixels/byte, 1:1 (640×200)

## Z80 undocumented instructions

When a `DD` or `FD` prefix is followed by an opcode that references the `H` or `L` register (but not `(HL)`), the real Z80 silently substitutes `IXH`/`IXL` or `IYH`/`IYL` respectively. Zilog never documented this, but it is consistent on all real silicon and some software relies on it.

`exec_xy()` in `z80.c` handles all such cases:

- **LD r, r'** (0x40–0x7F, neither side `(HL)`): H/L map to XYH/XYL
- **ALU XYH/XYL** (0x84, 0x85, 0x8C, 0x8D, 0x94, 0x95, 0x9C, 0x9D, 0xA4, 0xA5, 0xAC, 0xAD, 0xB4, 0xB5, 0xBC, 0xBD)
- **INC/DEC XYH/XYL** (0x24, 0x25, 0x2C, 0x2D)
- **LD XYH/XYL, n** (0x26, 0x2E)

Truly unrecognised DD/FD opcodes (e.g. `DD DD`) still fall through: the prefix is treated as a NOP and the following byte is re-executed as a plain instruction.

## Palette flush fallback

The CPC firmware manages a cooperative interrupt handler that runs a "flush task" to push palette RAM (at 0xB7D4–0xB7E4) to the Gate Array after ink changes. Some games (e.g. Spindizzy) deactivate this task before writing their initial palette, relying on real-hardware interrupt timing to catch the update.

`cpc_frame()` includes a fallback: if the firmware's dirty flag (0xB7F7 = 0xFF) is still set at end-of-frame, all 17 ink values are flushed directly from palette RAM to the Gate Array. This is a no-op when the firmware's flush task ran normally (it clears the flag), and only activates when the firmware path was bypassed.

## Reset

`cpc_reset()` performs a warm reset: all chips are reinitialised (Z80, Gate Array, CRTC, PPI, PSG, keyboard) and raster counters are cleared, but ROM and RAM contents are preserved — matching real CPC hardware reset behaviour.

## Configuration

`config_load()` reads `~/.config/1984/1984.conf` (INI format). If the file is missing it is created with defaults. Invalid or missing values fall back to defaults silently; no value causes a hard failure.

`config_set_model()` sets the model, RAM size, and ROM paths together — used by the overlay when the user switches model so all three stay consistent.

## Options overlay

The overlay (`src/overlay.c`) is a lightweight immediate-mode UI rendered with `SDL_RenderDebugText` at 1.5× scale. It has three tabs:

| Tab | Rows |
|-----|------|
| General | Model, OS ROM path, BASIC ROM path |
| Storage | Drive A, Tape (stubs) |
| Advanced | Memory, M4, UliFAC, Net4CPC |

The overlay snapshots the Config struct on open. If the user changes any value and then closes (ESC or F9), a "Save changes?" dialog appears. Enter saves to disk; ESC reverts to the snapshot. Switching the model automatically updates RAM size and ROM paths via `config_set_model()`.

## Paste

`src/paste.c` queues clipboard text (set via `paste_text()`) and injects it into the CPC keyboard matrix one character at a time through `paste_tick()`, called once per frame before `cpc_frame()`.

Each character goes through a two-phase cycle: key-down for `HOLD_FRAMES` (2) frames, then key-up with a `GAP_FRAMES` (1) frame gap before the next character. At 50 Hz this gives ~60 ms per character. An initial 3-frame delay on paste start ensures the host Ctrl key has been released from the matrix before the first character is injected (Ctrl+V would otherwise produce Ctrl+key control codes).

The ASCII→CPC matrix mapping (`keymap[]`) covers a–z, A–Z (with shift), 0–9, common punctuation, and newline (→ Return). `\r` and unmapped characters are silently skipped. A trailing newline is always appended so a pasted BASIC line is automatically entered.

## Memory map

```
0x0000–0x3FFF   OS ROM (lower) or RAM
0x4000–0x7FFF   RAM
0x8000–0xBFFF   RAM
0xC000–0xFFFF   BASIC ROM (upper) or RAM bank (6128)
```

The 6128 has 128 KB RAM. Extra 16 KB pages are selected via Gate Array port 0x7Fxx (function bits 11xxxxxx).

## I/O decoding

| Address bits | Device | Port example |
|---|---|---|
| A15=0, A14=1 | Gate Array | 0x7Fxx |
| A14=0, A8=0 | CRTC select | 0xBCxx |
| A14=0, A8=1 | CRTC write/read | 0xBDxx / 0xBFxx |
| A11=0 | PPI (8255) | 0xF4–0xF7xx |
| hi=0xFA | FDC motor control | 0xFAxx |
| hi=0xFB | FDC status / data | 0xFB7E / 0xFB7F |

# 1984 CPC Emulator — Development Notes

Architecture and implementation details for contributors.
For usage see [README.md](README.md).

---

## Architecture

Each source file maps to one hardware component:

| File | Component |
|------|-----------|
| `src/z80.c` / `z80.h` | Z80 CPU — full documented instruction set plus undocumented IX/IY half-register ops, all prefixes (CB/DD/ED/FD), interrupts IM0/1/2 |
| `src/mem.c` / `mem.h` | Memory map — lower/upper ROM overlay, 32 expansion ROM slots, 6128 RAM banking via Gate Array, configurable RAM 64–576 KB |
| `src/crtc.c` / `crtc.h` | MC6845 CRTC — horizontal/vertical timing, MA/RA address generation, display enable |
| `src/gate_array.c` / `gate_array.h` | Gate Array — screen mode, ink palette (32 hardware colours), ROM enables, interrupt counter |
| `src/ppi.c` / `ppi.h` | 8255 PPI — keyboard row selection, vsync feedback, PSG control routing |
| `src/psg.c` / `psg.h` | AY-3-8912 PSG — tone (3 channels), noise (17-bit LFSR), envelope generator, logarithmic volume table; renders 882 samples per frame into an SDL3 audio stream |
| `src/kbd.c` / `kbd.h` | Keyboard matrix — SDL scancode → CPC row/column mapping |
| `src/display.c` / `display.h` | SDL3 display — 768×272 pixel buffer, letterboxed into the window at 4:3 aspect |
| `src/disk.c` / `disk.h` | DSK disk image parser — track/sector layout, AMSDOS directory, read |
| `src/fdc.c` / `fdc.h` | µPD765 FDC — command/exec/result phases, READ DATA, SEEK, SENSE INTERRUPT STATUS |
| `src/rtc.c` / `rtc.h` | DS12887 RTC — register read/write, NVRAM, time from host `localtime()` |
| `src/cpc.c` / `cpc.h` | Top-level machine — bus wiring, frame execution, pixel rendering, reset |
| `src/config.c` / `config.h` | INI config file — load/save `~/.config/1984/1984.conf`, first-run creation, model defaults |
| `src/overlay.c` / `overlay.h` | SDL3 in-app options overlay — tabbed menu, dirty tracking, save-on-close prompt |
| `src/paste.c` / `paste.h` | Host-to-emulator paste — queues clipboard text and injects keypresses into the CPC matrix |
| `src/monitor.c` / `monitor.h` | Memory monitor / debugger — 80×25 terminal window, commands, PTY interface |
| `src/z80dis.c` / `z80dis.h` | Z80 disassembler — standalone, no external dependencies |
| `src/joy.c` / `joy.h` | Joystick/gamepad input — SDL gamepad + raw joystick fallback, hot-plug |
| `src/main.c` | Entry point — SDL init, event loop, F5/F9/F12/Ctrl+V handling |

---

## Render pipeline

The frame render is split into two phases to allow the overlay to composite on top of the CPC video output:

1. **`cpc_frame()`** — runs the CPU and CRTC for one PAL frame (80,000 cycles), writes pixels into `display.pixels[]`, then renders 882 audio samples from the PSG and pushes them to the SDL audio stream
2. **`display_upload()`** — uploads the pixel buffer to the SDL texture, clears the renderer, and blits the texture letterboxed into the window
3. **`overlay_render()`** — draws the overlay on top of the renderer (if visible), using `SDL_SetRenderScale` at 1.5× for the bitmap font
4. **`display_flip()`** — calls `SDL_RenderPresent`

---

## Timing

The CPU runs at 4 MHz. The CRTC ticks at 1 MHz (one character clock every 4 CPU cycles). Each character clock outputs 16 pixels. A PAL frame is 50 Hz (80,000 CPU cycles). VSYNC rising edge triggers `display_upload()`.

---

## Video rendering

The pixel buffer is 768×272. The CRTC's MA and RA registers are used to compute the video RAM address:

```
addr = (MA[13:12] << 14) | (RA[2:0] << 11) | (MA[9:0] << 1)
```

Two bytes are fetched per character clock and decoded according to the Gate Array screen mode:

- **Mode 0** — 4 bpp, 2 pixels/byte, each pixel 4× wide (160×200)
- **Mode 1** — 2 bpp, 4 pixels/byte, each pixel 2× wide (320×200)
- **Mode 2** — 1 bpp, 8 pixels/byte, 1:1 (640×200)

---

## Memory map

```
0x0000–0x3FFF   OS ROM (lower) or RAM
0x4000–0x7FFF   RAM
0x8000–0xBFFF   RAM
0xC000–0xFFFF   Upper ROM (slot selected by port 0xDFxx) or RAM bank
```

RAM is configurable from 64 KB (464 minimum) up to 576 KB (DK'tronics ceiling) via the options overlay. The physical array in `Mem` is always 576 KB; `Mem.ram_size` (set from `config.memory_kb * 1024`) controls how much of it is accessible. Accesses beyond `ram_size` return 0xFF and writes are silently dropped.

**Banking.** When the Gate Array receives a byte with bits[7:6] = `11` (port 0x7Fxx), it is a RAM banking command. `Mem.ram_bank` stores bits[5:0] of that byte. Both the 464 and 6128 honour this command; on real hardware only the 6128 supports it, but the emulator enables banking on the 464 as well when RAM is expanded beyond 64 KB.

The 8 banking modes (bits[2:0]) use a fixed page-to-region lookup table (not a raw page index). Bits[5:3] select a 64 KB expansion bank for DK'tronics-compatible expansion. The standard DK'tronics hardware supports up to 576 KB: 64 KB base (romb0–3) + 8 expansion banks × 64 KB (RAM_bank 0–7, where bank 0 is the standard 6128 extra 64 KB).

**Upper ROM slots.** `mem_read()` resolves the upper ROM in priority order:

1. Expansion override (`rom_ext[slot]`) — if loaded, wins for any slot 0–31
2. Slot 0 default → `rom_basic` (Locomotive BASIC)
3. Slot 7 default → `rom_amsdos` (AMSDOS, if present)
4. All other slots → 0xFF (no ROM)

The 32-slot `rom_ext[]` array allows loading any expansion ROM at any slot without touching the built-in BASIC or AMSDOS buffers.

---

## I/O decoding

| Address bits | Device | Port example |
|---|---|---|
| A15=0, A14=1 | Gate Array | 0x7Fxx |
| A14=0, A8=0 | CRTC select | 0xBCxx |
| A14=0, A8=1 | CRTC write/read | 0xBDxx / 0xBFxx |
| A11=0 | PPI (8255) | 0xF4–0xF7xx |
| hi=0xFA | FDC motor control | 0xFAxx |
| hi=0xFB | FDC status / data | 0xFB7E / 0xFB7F |
| hi=0xFD, lo=0x14 | RTC data (DS12887) | 0xFD14 |
| hi=0xFD, lo=0x15 | RTC address (DS12887) | 0xFD15 |
| hi=0xFD, lo=0x20–0x23 | Net4CPC W5100S | 0xFD20–0xFD23 |

---

## Z80 undocumented instructions

When a `DD` or `FD` prefix is followed by an opcode that references the `H` or `L` register (but not `(HL)`), the real Z80 silently substitutes `IXH`/`IXL` or `IYH`/`IYL` respectively. Zilog never documented this, but it is consistent on all real silicon and some software relies on it.

`exec_xy()` in `z80.c` handles all such cases:

- **LD r, r'** (0x40–0x7F, neither side `(HL)`): H/L map to XYH/XYL
- **ALU XYH/XYL** (0x84, 0x85, 0x8C, 0x8D, 0x94, 0x95, 0x9C, 0x9D, 0xA4, 0xA5, 0xAC, 0xAD, 0xB4, 0xB5, 0xBC, 0xBD)
- **INC/DEC XYH/XYL** (0x24, 0x25, 0x2C, 0x2D)
- **LD XYH/XYL, n** (0x26, 0x2E)

Truly unrecognised DD/FD opcodes (e.g. `DD DD`) still fall through: the prefix is treated as a NOP and the following byte is re-executed as a plain instruction.

---

## Palette flush fallback

The CPC firmware manages a cooperative interrupt handler that runs a "flush task" to push palette RAM (at 0xB7D4–0xB7E4) to the Gate Array after ink changes. Some games (e.g. Spindizzy) deactivate this task before writing their initial palette, relying on real-hardware interrupt timing to catch the update. With our instant FDC, the task can terminate before the game's palette write, leaving the GA all-black.

`cpc_frame()` includes a fallback: if the firmware's dirty flag (0xB7F7 = 0xFF) is still set at end-of-frame and a firmware write to the palette buffer is pending, all 17 ink values are flushed directly from palette RAM to the Gate Array.

**Gating the fallback.** Programs that write directly to the Gate Array (bypassing the firmware ink routine) — such as diagnostic ROMs that also run RAM tests over the firmware workspace — would otherwise corrupt the GA palette if the fallback ran unconditionally. The fallback is gated by `CPC.firmware_palette_count`, which:

- Starts incrementing each frame when lower-ROM-enabled code writes `0xFF` to `0xB7F7` and B7F7 remains `0xFF` at end-of-frame.
- Resets to zero when any non-`0xFF` value is written to `0xB7F7` (test pattern, flush-task clear).
- Fires the fallback when the count reaches `PALETTE_FLUSH_FRAMES` (50).

The 50-frame threshold exceeds the longest known consecutive-0xFF run produced by a RAM test (AmstradDiag 1.4L's 0xFF fill lasts 38 frames before the next test pattern is written). Spindizzy holds `0xB7F7=0xFF` indefinitely, so it fires well within the 1-second delay.

RAM-resident programs running with lower ROM disabled cannot arm the counter at all.

---

## PSG / audio

The AY-3-8912 is clocked at 1 MHz (CPU clock ÷ 4). `psg_render()` is called once per frame and generates 882 mono samples (44 100 Hz ÷ 50 Hz) which are pushed to an SDL3 audio stream.

**Oversampling.** `psg_tick()` advances all three generators by one AY clock. `psg_render()` calls it ~22–23 times per output sample (the exact count varies with the fractional accumulator `clock_rem`) and averages the results. This acts as a box-filter anti-alias on the square waves.

**Fractional clock accumulator.** `clk_per_sample = 1 000 000 / 44 100 = 22.676…`. A `float clock_rem` carries the fraction across samples so pitch is exact rather than drifting ~3% from integer truncation.

**AY ÷8 prescaler.** The real AY chip has an internal ÷8 prescaler before its tone, noise, and envelope counters. All counter thresholds are therefore multiplied by 8 in `psg_tick()`, so that register values from real CPC software work without adjustment (e.g. `SOUND 1,142,…` plays A4 at 440 Hz, not 3 octaves too high). The formula for tone frequency is: `f = 1 MHz / (16 × N)`.

**Envelope.** 32 steps per cycle; each step advances every `ep × 8` AY clocks (where `ep = (R12 << 8) | R11`). Shape bits CONT/ALT/HOLD (R13 bits 3/1/0) select between single-shot, sawtooth, and triangle modes. Writing R13 resets the envelope. `env_counter` is `u32` because `ep × 8` can reach 524 280.

**Low-pass filter.** A one-pole IIR (`lp = (x + lp) >> 1`) is applied after oversampling. This gives a ~7 kHz cutoff at 44 100 Hz, removing the high-frequency aliasing that makes CPC square waves sound metallic.

**Frame pacing.** VSync is disabled (`SDL_SetRenderVSync(renderer, 0)`). The main loop uses `SDL_GetTicksNS()` / `SDL_DelayNS()` to sleep for whatever is left of each 20 ms budget. This prevents the display refresh rate (typically 60 Hz) from overdriving the audio queue and causing growing latency.

---

## Reset

`cpc_reset()` performs a warm reset: all chips are reinitialised (Z80, Gate Array, CRTC, PPI, PSG, keyboard) and raster counters are cleared, but ROM and RAM contents are preserved — matching real CPC hardware reset behaviour.

The overlay triggers a **cold boot** (ROM reload + `cpc_reset`) automatically when any of the following are saved: model change, RAM size change, DD1 toggle, lower ROM replacement, or any expansion ROM slot change. ROM data and `Mem.ram_size` are updated in-place before the reset so the machine immediately boots with the new configuration without restarting the emulator.

---

## Configuration

`config_load()` reads `~/.config/1984/1984.conf` (INI format). If the file is missing it is created with defaults. Invalid or missing values fall back to defaults silently; no value causes a hard failure.

`config_set_model()` sets the model, RAM size, and ROM paths together — used by the overlay when the user switches model so all three stay consistent.

`config_default_os/basic/amsdos()` return the compiled-in default ROM path for a given model, used by the overlay's Delete action to restore individual ROM slots to their factory defaults without touching the rest of the config.

`config_apply_dd1()` sets or clears the AMSDOS ROM path and the `dd1` flag together, keeping them consistent. Called by the overlay when DD1 is toggled and by `config_set_model()` when switching to 464.

`config_default_diag()` returns the compiled-in path for `AmstradDiagLower.rom` (`~/.config/1984/roms/AmstradDiagLower.rom`). Used by the overlay's Diag Cart toggle to resolve the ROM path without hardcoding the filename in the UI layer.

**`memory_kb`** accepts 64, 128, 256, 512, or 576. The 464 default is 64; the 6128 default is 128. Values outside this set are rejected and the previous value is kept. The field is written directly to `Mem.ram_size` (in bytes) at startup and on every cold boot.

**CLI ROM slot overrides.** `--rom-slot=N:PATH` (repeatable) loads a ROM into slot N after the config-based expansion ROMs are applied. This lets you test or launch with a specific ROM without modifying `1984.conf`. CLI overrides win over config-file assignments for the same slot.

---

## Options overlay

The overlay (`src/overlay.c`) is a lightweight immediate-mode UI rendered with `SDL_RenderDebugText` at 1.5× scale. It has three tabs:

| Tab | Rows |
|-----|------|
| General | Model, OS ROM path, BASIC ROM path |
| Storage | Drive A, Drive B |
| Advanced | Memory, M4 [unimplemented], UliFAC [unimplemented], RTC, DD1, ROM Slots →, Diag Cart, Net4CPC |

The overlay snapshots the Config struct on open. If the user changes any value and then closes (ESC or F9), a "Save changes?" dialog appears. Enter saves to disk; ESC reverts to the snapshot. Switching the model automatically updates RAM size and ROM paths via `config_set_model()`.

The **Memory** row (Advanced tab, row 0) cycles through 64 → 128 → 256 → 512 → 576 KB on Enter for both the 464 and 6128. A memory change sets `dirty = true` and, on save, adds `memory_kb != saved.memory_kb` to the cold boot trigger so `main.c` updates `Mem.ram_size` before calling `cpc_reset()`.

**ROM Slots sub-panel** (`OV_STATE_ROMSLOTS`) is opened from Advanced → ROM Slots. It shows the lower ROM and all 32 upper ROM slots (panel indices 0–32, where 0 = lower ROM and 1–32 = upper slots 0–31) with 10 entries visible at a time. Enter opens a file picker (`SDL_ShowOpenFileDialog`); Delete restores the slot to its compiled-in default (for Lower ROM, Slot 0, Slot 7) or clears it (all others). Any change sets `needs_cold_boot` on the overlay; `main.c` checks this flag after `overlay_tick()`, reloads the affected ROMs, and calls `cpc_reset()`.

---

## Paste

`src/paste.c` queues clipboard text (set via `paste_text()`) and injects it into the CPC keyboard matrix one character at a time through `paste_tick()`, called once per frame before `cpc_frame()`.

Each character goes through a two-phase cycle: key-down for `HOLD_FRAMES` (2) frames, then key-up with a `GAP_FRAMES` (1) frame gap before the next character. At 50 Hz this gives ~60 ms per character. An initial 3-frame delay on paste start ensures the host Ctrl key has been released from the matrix before the first character is injected (Ctrl+V would otherwise produce Ctrl+key control codes).

The ASCII→CPC matrix mapping (`keymap[]`) covers a–z, A–Z (with shift), 0–9, common punctuation, and newline (→ Return). `\r` and unmapped characters are silently skipped. A trailing newline is always appended so a pasted BASIC line is automatically entered.

---

## DS12887 Real-Time Clock (`src/rtc.c`)

The RTC emulates a DS12887 as used in the Cyboard and Symbiface II CPC add-ons. It is enabled via `rtc=true` in `1984.conf` (or the Advanced → RTC toggle in the overlay). The toggle does not require a cold boot.

**Port mapping:**

| Port | Direction | Function |
|------|-----------|----------|
| 0xFD15 | write | Address register — selects DS12887 register 0x00–0x7F |
| 0xFD14 | read/write | Data register |

**Registers.** Time and date registers (0x00=sec, 0x02=min, 0x04=hour, 0x06=dow, 0x07=day, 0x08=month, 0x09=year, 0x32=century) always return the current host time from `localtime()`. Register B (0x0B) is the only control register stored persistently per-session; it controls the data format:

- Bit 2 (DM): 0 = BCD output, 1 = binary output
- Bit 1 (24/12): 0 = 12h mode (PM bit = bit 7 of hours register), 1 = 24h mode

`regb` defaults to `0x02` (24h BCD) so the first read before any software sets Register B returns valid BCD hours without a PM bit.

**NVRAM.** Registers 0x0E–0x7F are 114 bytes of battery-backed RAM. The `RTC` struct holds a `nvram[114]` array; writes are stored and reads return the stored value. This is required because SymbOS performs a write-then-read battery check on NVRAM before trusting the RTC: it writes a signature byte to register 0x0F, reads it back, and only reads the time if the values match. Without NVRAM storage, the check would always fail and SymbOS would ignore the hardware clock.

**SymbOS boot sequence.** SymbOS checks its `SYMBOS.INI` hardware flags byte (file offset 0x240, bit 1 = RTC present). If set, it:
1. Sets RegB = 0x07 (binary mode, 24h, DSE enabled)
2. Writes a signature to NVRAM reg 0x0F and reads it back (battery check)
3. Reads RegA (0x0A) to confirm UIP=0 (no update in progress)
4. Reads all time/date registers in binary 24h format
5. Sets RegB bit 7 (SET=1) to freeze the clock, writes the time back, then clears SET

**Writes to time/date registers** are accepted but silently discarded — the host clock is always the authoritative source. Step 5 above is therefore a no-op on the emulated chip.

---

## Joystick / input

Joystick input is mapped to CPC keyboard matrix row 9 (the hardware joystick row). Column assignments: 0 = Up, 1 = Down, 2 = Left, 3 = Right, 4 = Fire 1, 5 = Fire 2.

### SDL event handling (`src/joy.c`)

SDL3 controllers are handled in two tiers:

- **Gamepad** (`SDL_EVENT_GAMEPAD_*`): devices in the SDL gamepad database are opened with `SDL_OpenGamepad`. D-pad and face buttons map directly to the row-9 columns; the left analogue stick uses a ±8000 dead zone to derive digital Up/Down/Left/Right.
- **Raw joystick** (`SDL_EVENT_JOYSTICK_*`): devices not in the gamepad database fall back to `SDL_OpenJoystick`. Axis 0 → Left/Right, axis 1 → Up/Down; button 0 → Fire 1, button 1 → Fire 2; hat switch is also handled.

Both tiers support hot-plug via the `ADDED`/`REMOVED` events. Up to `JOY_MAX_PADS` of each type can be open simultaneously; only the first active controller generates CPC keypresses.

**Background events.** SDL3 defaults to suppressing gamepad and joystick events when the SDL window does not have focus. `SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1")` is called before `SDL_Init` so events are always delivered.

Use `--trace-input` to log all keyboard and joystick events to stderr.

### PSG keyboard scan (`src/cpc.c` / `src/psg.c`)

The CPC reads the keyboard matrix (including the joystick row) through PSG I/O port A (register 14). The scan sequence driven by software:

1. Write the row number to PPI port C bits 3–0 (`ppi.kbd_row`).
2. Set PPI port C bits 7–6 = `11` (PSG latch mode) with the desired register number on PPI port A → calls `psg_select()`.
3. Set PPI port C bits 7–6 = `01` (PSG read mode) → the emulator calls `psg_set_kbd_row()` then returns the row data via PPI port A.

In step 3, `psg_ctrl == 0x01` always means the CPU is reading the keyboard through I/O port A (register 14). The code reads `psg->kbd_data` directly rather than going through `psg_read()`, which would only return `kbd_data` if `psg->selected == 14`. This avoids a bug where software that relies on register 14 being implicitly selected (e.g. SymbOS) would receive `psg->reg[0] = 0x00` — all keys falsely "pressed" — instead of the real matrix data.

---

## Memory Monitor / Debugger (F8)

Pressing **F8** opens (or closes) a separate SDL window: an 80×25 green-phosphor terminal with a `>_` prompt. All commands are also accessible via the PTY serial interface (`--monitor-pty`).

### Window layout

| Row(s) | Content |
|--------|---------|
| 0 – 22 | Scrolling output (23 lines) |
| 23     | Live register status bar |
| 24     | Command-input prompt (`>_`) |

Window size: **960 × 720** (4:3). Achieved with a non-uniform SDL render scale: `FONT_SCALE_X = 1.5`, `FONT_SCALE_Y = 3.6`, giving 12 × 28.8 px per character.

### Register status bar (row 23)

Rendered live on every frame from `CPC.cpu` — never stored in the screen buffer. Shows:

```
PC:XXXX SP:XXXX A:XX F:SZ-H-PNC BC:XXXX DE:XXXX HL:XXXX IX:XXXX IY:XXXX [PAUSED]
```

Background colour: dark green when running, dark red when paused.

### Commands

| Command | Description |
|---------|-------------|
| `D <addr> [<end>]` | Disassemble Z80. Without end address, prints 10 lines. Pageable with ENTER/SPACE. |
| `M <addr> [<end>]` | Hex + ASCII dump. Without end address, fills the screen. ASCII column is shown in reverse video. |
| `B [<addr>]` | Set a breakpoint at `addr`, or list all active breakpoints. |
| `BC <n>` | Clear breakpoint slot `n` (0 – 15). |
| `N` | Single-step one Z80 instruction (emulator must be paused). |
| `G` / `GO` | Resume execution (clear pause). |
| `GA` | Dump Gate Array: screen mode, border ink, all 16 inks. |
| `CRTC` | Dump all 18 CRTC registers plus live counters (MA, VLC, HCC, VCC). |
| `X` / `Q` | Close the monitor window. |

---

## Z80 Disassembler (`src/z80dis.c`)

Standalone C file, no external dependencies. API:

```c
int z80dis(const u8 *mem, u16 pc, char *out, size_t outsz);
// Returns bytes consumed; out receives e.g. "LD HL,0B900h"
```

Decodes using the standard Z80 bit-field decomposition (`x = op>>6`, `y = (op>>3)&7`, `z = op&7`) plus all prefix paths (CB, DD, ED, FD, DDCB, FDCB). The DDCB/FDCB path reads the displacement byte *before* the final opcode byte.

---

## Breakpoints & Single-Step (`src/cpc.h` / `src/cpc.c`)

### CPC struct additions

```c
#define CPC_MAX_BREAKPOINTS 16
bool paused;
bool step_once;
u16  breakpoints[CPC_MAX_BREAKPOINTS];
bool bp_enabled[CPC_MAX_BREAKPOINTS];
```

### `cpc_frame()` logic

```
if paused && !step_once → return immediately (emulator frozen)
clear step_once; record was_stepping

inner loop (per Z80 instruction):
  z80_step()
  CRTC ticks + pixel rendering
  GA interrupt delivery
  check breakpoints → if hit: paused=true, stop_early=true, break
  if stop_early || was_stepping → break

cycle_debt = stop_early ? 0 : (done – target)
increment frame counter
flush palette buffers
if !stop_early → push audio frame to SDL
```

When `stop_early` is true, `cycle_debt` is zeroed to prevent the negative debt from skewing the next frame after resume. Audio is skipped to avoid a burst of silence samples when the frame is partial.

### Auto-open on breakpoint

`main.c` checks the pause state before and after `cpc_frame()`:

```c
bool was_paused   = cpc.paused;
bool was_stepping = cpc.step_once;
cpc_frame(&cpc);
if (!was_paused && cpc.paused) {
    monitor_open(monitor);
    monitor_notify_break(monitor);   // prints "*** Breakpoint at PC=XXXX ***"
                                      // + 5-line disassembly at PC
} else if (was_stepping && cpc.paused) {
    monitor_notify_step(monitor);    // prints 1-line disassembly at new PC
}
```

---

## PTY Serial Interface (`--monitor-pty`)

Starting the emulator with `--monitor-pty` opens a POSIX PTY master (`posix_openpt`), configures it at 9600 baud raw, and prints the slave device path to stderr:

```
1984: monitor PTY: /dev/pts/5  (minicom -b 9600 -D /dev/pts/5)
```

The PTY exposes the full command set. Output lines that contain reverse-video ASCII (hex dump) are wrapped with ANSI `\033[7m` … `\033[0m` escape sequences.

`monitor_pty_tick()` is called once per frame from `main.c`. It reads up to 64 bytes per call (non-blocking), handles line editing (backspace), echoes characters, and executes lines on `\r` or `\n`.

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
| `src/ide.c` / `ide.h` | ATA PIO IDE — SYMBiFACE II / Cyboard port mapping, raw disk image backend, IDENTIFY/READ/WRITE |
| `src/mouse.c` / `mouse.h` | SYMBiFACE II PS/2 mouse — port 0xFD10, variable-length burst protocol, SDL relative mouse capture |
| `src/ch376.c` / `ch376.h` | Albireo CH376 USB host controller — ports 0xFE80/0xFE81, file-system command set backed by `src/fat.c`; mutually exclusive with M4 (shared 0xFExx decode) |
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

**AMSDOS-headed ROMs.** All ROM loaders (`mem_load_os`, `mem_load_rom`, `mem_load_amsdos`, `mem_load_rom_ext`) go through a `read_rom_image()` helper. If the file is exactly `ROM_BASIC_SIZE + 128` bytes (16512), the 128-byte AMSDOS header is skipped and only the 16384-byte ROM body is loaded. This handles ROMs distributed in DSK-extracted form (e.g. UNIDOS, UNITOOLS, Albireo) transparently — no per-device unwrapping needed.

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
| hi=0xFD, lo=0x06 | IDE Alt Status / Device Control | 0xFD06 |
| hi=0xFD, lo=0x08–0x0F | IDE task-file registers (Data … Command) | 0xFD08–0xFD0F |
| hi=0xFD, lo=0x10 | PS/2 mouse (SYMBiFACE II) | 0xFD10 |
| hi=0xFD, lo=0x14 | RTC data (DS12887) | 0xFD14 |
| hi=0xFD, lo=0x15 | RTC address (DS12887) | 0xFD15 |
| hi=0xFD, lo=0x20–0x23 | Net4CPC W5100S | 0xFD20–0xFD23 |
| hi=0xFE, lo=0x80/0x81 | Albireo CH376 data / command-status | 0xFE80 / 0xFE81 |
| hi=0xFE or 0xFF (M4 only) | M4 data port (broad decode, takes precedence over Albireo only when Albireo is disabled) | 0xFExx / 0xFFxx |

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

The overlay triggers a **cold boot** (ROM reload + `cpc_reset`) automatically when any of the following are saved: model change, RAM size change, DD1 toggle, lower ROM replacement, any expansion ROM slot change, or any change to the SYMBiFACE IDE enable state or image path. ROM data and `Mem.ram_size` are updated in-place before the reset so the machine immediately boots with the new configuration without restarting the emulator.

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
| General | Model, Memory, MX4, Roms Board, OS ROM, BASIC ROM |
| Media | Drive A, Drive B |
| Extensions | M4, UliFAC [unimplemented], Net4CPC, RTC, DD1, ROM Slots →, Diag Cart, SYMBiFACE IDE, SYMBiFACE Mouse, Albireo, Cyboard |

The overlay snapshots the Config struct on open. If the user changes any value and then closes (ESC or F9), a "Save changes?" dialog appears. Enter saves to disk; ESC reverts to the snapshot. Switching the model automatically updates RAM size and ROM paths via `config_set_model()`.

The **Memory** row (General tab, row 1) cycles through 64 → 128 → 256 → 512 → 576 → 768 → 1024 KB on Enter for both the 464 and 6128. A memory change sets `dirty = true` and, on save, adds `memory_kb != saved.memory_kb` to the cold boot trigger so `main.c` updates `Mem.ram_size` before calling `cpc_reset()`.

The **Roms Board** row (General tab, row 3) toggles `cfg.rom_board`. When false, `main.c`'s boot and cold-boot paths skip the loop that copies `cfg.rom_ext[]` into the upper ROM slots — only the model's default OS, BASIC, and AMSDOS ROMs are loaded. The `cfg.rom_ext[]` paths themselves are *not* cleared, so re-enabling the toggle restores the previous layout from a single source of truth. The Extensions → ROM Slots row is also rendered as `[disabled]` and its sub-panel is unreachable while `rom_board` is off. Triggers a cold boot on save.

When `rom_board=false`, the four ROM-backed expansions (M4, SYMBiFACE IDE, SYMBiFACE Mouse, Albireo) are forced off in the live CPC state at boot/cold-boot time — `cpc.m4 = cfg.m4 && cfg.rom_board`, and likewise for the others. The cfg values stay intact so re-enabling Roms Board restores them. In the overlay, those four rows (plus the Cyboard combo, which contains two ROM-backed peripherals) render as `[needs Roms Board]` and their `activate_item()` paths short-circuit early. Net4CPC and RTC are unaffected because they don't need a companion ROM driver.

The **MX4** row (General tab, row 2) toggles `cfg.mx4`, which gates the expansion-bus dispatch in `cpc.c` — every `0xFDxx` / `0xFExx` / `0xFFxx` extension branch in `bus_io_read`/`bus_io_write` is conditional on `cpc->mx4`. When MX4 is off, the **Extensions** tab is hidden from the top bar entirely (the render loop skips `i == OV_ADVANCED`, and the `LEFT`/`RIGHT` navigation does a `do/while` that steps past `OV_ADVANCED`). Changing MX4 triggers a cold boot on save so the CPC firmware re-probes the bus.

The **OS ROM** and **BASIC ROM** rows (General tab, rows 4 and 5) open `SDL_ShowOpenFileDialog` on Enter, with new dialog kinds `DIALOG_LOWER_ROM` (reused from the ROM Slots panel) and `DIALOG_BASIC_ROM`. The dialog callback writes the chosen path into `cfg.rom_os` / `cfg.rom_basic` and sets `needs_cold_boot` so the next save reloads the ROMs via `cpc_init`/`mem_load_rom`. The value column shows the basename only.

**ROM Slots sub-panel** (`OV_STATE_ROMSLOTS`) is opened from Extensions → ROM Slots. It shows the lower ROM and all 32 upper ROM slots (panel indices 0–32, where 0 = lower ROM and 1–32 = upper slots 0–31) with 10 entries visible at a time. Enter opens a file picker (`SDL_ShowOpenFileDialog`); Delete restores the slot to its compiled-in default (for Lower ROM, Slot 0, Slot 7) or clears it (all others). Any change sets `needs_cold_boot` on the overlay; `main.c` checks this flag after `overlay_tick()`, reloads the affected ROMs, and calls `cpc_reset()`.

---

## Paste

`src/paste.c` queues clipboard text (set via `paste_text()`) and injects it into the CPC keyboard matrix one character at a time through `paste_tick()`, called once per frame before `cpc_frame()`.

Each character goes through a two-phase cycle: key-down for `HOLD_FRAMES` (2) frames, then key-up with a `GAP_FRAMES` (1) frame gap before the next character. At 50 Hz this gives ~60 ms per character. An initial 3-frame delay on paste start ensures the host Ctrl key has been released from the matrix before the first character is injected (Ctrl+V would otherwise produce Ctrl+key control codes).

The ASCII→CPC matrix mapping (`keymap[]`) covers a–z, A–Z (with shift), 0–9, common punctuation, and newline (→ Return). `\r` and unmapped characters are silently skipped. A trailing newline is always appended so a pasted BASIC line is automatically entered.

---

## DS12887 Real-Time Clock (`src/rtc.c`)

The RTC emulates a DS12887 as used in the Cyboard and Symbiface II CPC add-ons. It is enabled via `rtc=true` in `1984.conf` (or the Extensions → RTC toggle in the overlay). The toggle does not require a cold boot.

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

## SYMBiFACE II / Cyboard IDE (`src/ide.c`)

The IDE emulator implements ATA PIO mode compatible with the SYMBiFACE II and Cyboard CPC add-ons. It is enabled via `symbiface_ide=true` in `1984.conf` (or Extensions → SYMBiFACE IDE in the overlay). The backend is a raw disk image opened with `fopen("r+b")`; the FAT filesystem is handled entirely by the guest OS driver.

**Port mapping** (confirmed against HDCPM.ROM disassembly):

| Port | Direction | ATA register |
|------|-----------|-------------|
| 0xFD06 | read | Alternate Status (no side effects) |
| 0xFD06 | write | Device Control (bit 2 = SRST) |
| 0xFD08 | read/write | Data |
| 0xFD09 | read | Error |
| 0xFD09 | write | Features |
| 0xFD0A | read/write | Sector Count |
| 0xFD0B | read/write | LBA Low |
| 0xFD0C | read/write | LBA Mid |
| 0xFD0D | read/write | LBA High |
| 0xFD0E | read/write | Device / Head |
| 0xFD0F | read | Status |
| 0xFD0F | write | Command |

**Supported commands:**

| Command | Opcode | Notes |
|---------|--------|-------|
| IDENTIFY DEVICE | 0xEC | Returns 512-byte block; ATA strings are byte-swapped per spec; word 49 sets LBA capability |
| READ SECTORS | 0x20 / 0x21 | LBA28; `fseeko` + `fread` per sector; multi-sector auto-advances LBA |
| WRITE SECTORS | 0x30 / 0x31 | LBA28; `fseeko` + `fwrite` + `fflush` per sector |
| INITIALIZE DRIVE PARAMETERS | 0x91 | Accepted silently |
| SET FEATURES | 0xEF | Accepted silently |

**LBA addressing.** 28-bit LBA is assembled from the four task-file registers: `lba = lba_low | (lba_mid << 8) | (lba_high << 16) | ((device & 0x0F) << 24)`.

**Reset behaviour.** `ide_reset()` (called on SRST and on `cpc_reset()`) preserves the open file pointer and sector count, then zeroes all ATA task-file registers. This keeps the drive connected across warm CPC resets. `ide_init()` (called once at startup) zeroes everything including the file pointer; `ide_open()` / `ide_close()` manage the file lifetime.

---

## SYMBiFACE II PS/2 Mouse (`src/mouse.c`)

The mouse emulator implements the SYMBiFACE II protocol at port 0xFD10 (read-only). It is enabled via `symbiface_mouse=true` in `1984.conf` (or Extensions → SYMBiFACE Mouse in the overlay). The toggle triggers a cold boot on save — SymbOS's input drivers only re-probe the hardware at boot, so toggling without a reset would leave the guest unaware of the change.

**Protocol.** The CPC reads port 0xFD10 in a tight polling loop until it receives `0x00` (no more data for this cycle). The port only emits packets for data that has actually changed since the last burst, which means a stationary mouse with no button activity returns `0x00` immediately.

| Byte range | Bits 7:6 | Bits 5:0 | Meaning |
|---|---|---|---|
| `0x00` | `00` | — | No more data; end polling loop |
| `0x40`–`0x7F` | `01` | signed 6-bit X | X offset; positive = right |
| `0x80`–`0xBF` | `10` | signed 6-bit Y | Y offset; positive = up |
| `0xC0`–`0xDF` | `11`, bit5=0 | button bits | bit0=left, bit1=right, bit2=middle |
| `0xE0`–`0xFF` | `11`, bit5=1 | signed 5-bit scroll | scroll wheel; negative = down |

**State machine.** The `Mouse` struct holds accumulated raw deltas (`dx`, `dy`, `dz`) and a `btn_changed` flag. When `mouse_read()` is called with `state == 0` (idle / start of burst), it snapshots all accumulated values, clears the accumulators, and advances through states 1–4 (X → Y → buttons → scroll), skipping any slot that carries no data. It returns `0x00` once all slots are exhausted and resets to state 0.

**SDL integration.** When captured, `SDL_EVENT_MOUSE_MOTION` delivers `xrel`/`yrel` (relative deltas; SDL Y is positive-downward, so Y is negated before storing). `SDL_EVENT_MOUSE_BUTTON_DOWN/UP` update the button state and set `btn_changed`. `SDL_EVENT_MOUSE_WHEEL` accumulates `wheel.y` into `dz`.

**Capture flow.** Clicking the emulator window calls `SDL_SetWindowRelativeMouseMode(win, true)`, hides the cursor, and updates the window title. Pressing **Ctrl+Enter** releases capture. Opening the overlay (F9) also releases capture automatically before the overlay takes focus.

**Capture gating.** Mouse capture engages only when `symbiface_mouse` is enabled. Albireo on its own does not arm the capture trigger — in the current build SymbOS's USB-HID enumeration on Albireo fails at the SETUP transfer (we return `USB_INT_DISCONNECT` since no real USB HID descriptor stack is emulated), so SymbOS falls back to its joystick-as-mouse driver. Capturing the host pointer for an Albireo-only setup would trap it without driving any guest mouse input.

---

## Net4CPC W5100S Ethernet (`src/net4cpc.c`)

The Net4CPC expansion exposes a WIZnet **W5100S** at four CPC I/O ports (`0xFD20`–`0xFD23`) in indirect-bus mode. The driver implements the full 64 KB W5100S register/buffer space (`regs[65536]`), four hardware sockets with their own 2 KB TX and 2 KB RX ring buffers, and the chip-level commands `OPEN`, `CONNECT`, `LISTEN`, `SEND`, `RECV`, `DISCON`, `CLOSE`. Socket operations are backed by host POSIX sockets.

**Port map and indirect addressing.** The host accesses the chip via two registers — `IDM_ARH` / `IDM_ARL` form a 16-bit address pointer, and `IDM_DR` reads or writes the data byte at that address. When MR bit 1 (AI, Address Auto-Increment) is set, `idm_ar` increments after every `IDM_DR` access — that's the only way to write multi-byte registers like `SHAR` (MAC, 6 bytes) or `SIPR` (4 bytes) without doing six separate `IDM_AR` writes.

**Software reset (MR.RST).** Writing `0x80` to `MR` (either via the direct shortcut at `0xFD20` or through `IDM_DR` with `idm_ar = 0x0000`) triggers a soft reset: every socket is closed (host-side `close(2)`), the entire register space is zeroed, then `MR` is set back to `0x03` (IND + AI). The post-reset MR value matters: real silicon mirrors the `BUS_SEL` pin's "indirect" state and leaves AI on by default, so the host can immediately write multi-byte registers afterwards. SymbOS's N4C daemon issues a soft reset as its first step and then polls `MR` until bit 7 clears; if we left `MR` at `0x00` after reset, every subsequent multi-byte write would silently clobber the same address (SHAR becomes `??:??:??:??:??:??` with only the last byte stored).

**Socket OPEN binds correctly.** SymbOS configures `Sn_PORT` (the local port) before issuing `OPEN`. The OPEN handler binds the host socket to `(SIPR or INADDR_ANY, Sn_PORT)`, sets `SO_REUSEADDR` so we can coexist with the host's own services if there's an overlap, and sets `SO_BROADCAST` on UDP sockets so `sendto(255.255.255.255)` is permitted (required for DHCP DISCOVER). Bind failures fall back to ephemeral port 0 rather than killing the socket.

**Limitations — no DHCP.** Because the emulator runs as a regular user-space process accessing the network through POSIX sockets rather than a raw L2 interface, broadcast DHCP exchanges cannot fully complete. We can *send* DHCP DISCOVER (with `SO_BROADCAST`), but server replies addressed to the emulated MAC and offered IP land at the host kernel, which silently drops them — the kernel doesn't own that IP. Use a **static IP configuration** with the N4C daemon (and any other Net4CPC consumer) until TUN/TAP support is added. With static config, DNS lookups over UDP, TCP connects, TCP send/receive, and HTTP fetches all work — verified end-to-end against `time.akamai.com` from SymbOS.

**Tracing.** `--trace-net4cpc` enables `net4cpc_trace = 1`. Every register access is logged with its decoded name (`MR`, `SHAR`, `SIPR`, `Sn_MR`, `Sn_DIPR`, etc.) and socket index where applicable. Socket commands are logged with mnemonic (`OPEN`, `CONNECT`, `SEND`, `RECV`, `CLOSE`). TX/RX ring-buffer accesses are summarised as `burst R/W [start..end] N bytes` to avoid spamming one line per byte. The MR shortcut read at `0xFD20` and `MR.RST` events get their own labelled lines.

---

## Albireo CH376 USB host (`src/ch376.c`)

The Albireo CPC expansion exposes a WCH **CH376** USB host controller. It is enabled via `albireo=true` in `1984.conf` (or Extensions → Albireo in the overlay). Enabling it from the overlay opens a file picker to choose a FAT16/FAT32 image; the path is stored in `albireo_image`. The backend is `src/fat.c` (shared with the M4 file API), so directory enumeration, open/read/write/seek, and free-space queries all go through the same FAT layer.

**Port map.** The CPC sees the chip at two I/O ports:

| Address | Direction | Purpose |
|---------|-----------|---------|
| `0xFE80` | r/w | DATA — command parameters and response payload |
| `0xFE81` | w   | COMMAND — start a command |
| `0xFE81` | r   | STATUS — bit7 = !INT (0 = interrupt pending), bit4 = BUSY (always 0 in emulation) |

**M4 mutex.** The real M4 board decodes the whole `0xFExx` / `0xFFxx` range as its data port, which would collide with the CH376 ports. In the emulator we route `0xFE80`/`0xFE81` to the CH376 first when `cpc->albireo` is set, but to keep configuration honest the overlay (and the boot-time config wiring in `main.c`) enforces mutual exclusion: enabling either Albireo or M4 disables the other and clears the corresponding image path. If a hand-edited config has both `albireo=true` and `m4=true`, Albireo wins.

**Command/response state machine.** The chip is driven by writing a command byte to `0xFE81`, then zero or more parameter bytes to `0xFE80`. Each command has a known fixed parameter count, or `-1` for variable-length (NUL-terminated) parameters used by `SET_FILE_NAME`. Once all parameters arrive, `execute()` runs.

Most commands raise an "interrupt" by setting `int_pending = true` and stashing a status code in `int_status`. The host polls `0xFE81` for bit 7 to clear, then issues `GET_STATUS` (0x22) which surfaces the status code on the data port and clears `int_pending`. Some commands (`CHECK_EXIST`, `GET_IC_VER`, `SET_USB_MODE`) return their result directly on the data port with no interrupt — UNIDOS's `CheckAlbireo` / `MountDevice` rely on the latter.

**One-shot byte vs. payload buffer.** Reads from `0xFE80` consult a one-shot single-byte register first (`oneshot` + `oneshot_valid`); commands like `GET_STATUS`, `CHECK_EXIST`, `GET_IC_VER`, and `SET_USB_MODE` write there. The `resp[]` buffer holds longer length-prefixed payloads such as the chunk from `BYTE_READ`, the 32-byte FAT directory entry served by `FILE_OPEN`/`FILE_ENUM_GO`/`DIR_INFO_READ`, the 4-byte `DISK_CAPACITY`, and the 9-byte `DISK_QUERY`. Separating the two prevents `GET_STATUS` from clobbering a pending chunk — without that split, UNIDOS's `BYTE_RD_GO` loop never terminates because the cached chunk length keeps coming back from the data port.

**File operations.** `SET_FILE_NAME` accumulates bytes until the NUL terminator, normalises backslashes to slashes and the path to uppercase, then stores it as the working filename. `FILE_OPEN` either opens an exact file via `fat_open` or, if the leaf contains `*`/`?`, starts an enumeration using `fat_opendir` + `fat_readdir` with a glob match. Each matched entry is composed into a synthetic 32-byte FAT8.3 directory entry (`make_dir_entry`) — the chip's built-in FAT layer presents these to the host even when the underlying disk uses long filenames. `BYTE_READ`/`BYTE_RD_GO` chunk reads through `do_byte_read`, always refreshing the length byte at `resp[0]` so an end-of-stream is signalled as length 0. `BYTE_WRITE`/`WR_REQ_DATA`/`BYTE_WR_GO` mirror the same flow for writes, flushing the host-supplied chunk to the FAT layer in `BYTE_WR_GO`.

**Disk introspection.** `DISK_CAPACITY` returns a length-prefixed 4-byte total-sector count. `DISK_QUERY` returns a length-prefixed 9-byte payload (`total_sectors[4] + free_sectors[4] + fs_type[1]`) — exactly the format UNIDOS's `GetDiskFreeSpace` expects (it asserts the length byte equals 9 and then `in`s the nine bytes via `inira`).

**Not implemented (yet):** `FILE_ERASE`, `DIR_INFO_SAVE`, the SC16C650B UART side at `0xFEB0–7`, and CH376 interrupts routed to NMI. UNIDOS polls the status register instead of relying on NMI delivery, so this last one is invisible to the guest.

**Tracing.** `--trace-albireo` enables `ch376_trace = 1`; every command is logged with its mnemonic and parameter bytes (or the filename for `SET_FILE_NAME`), and every interrupt status code is logged on the next line. This is the primary debugging aid for diagnosing UNIDOS command-flow regressions — most bugs found during development surfaced as either an unexpected status code or a payload mismatch visible in the trace.

---

## Cyboard master toggle

The **Cyboard** overlay item (Extensions → Cyboard, row 10) is a UI-only convenience; it writes to the four existing config flags rather than introducing a new one. Activating it when all four peripherals (Net4CPC, RTC, SYMBiFACE IDE, SYMBiFACE Mouse) are enabled disables all of them (and clears `ide_image`); activating it when any of the four is off enables all of them. The displayed value is `enabled` / `disabled` / `partial` depending on the combination.

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

---

## M4 board (`src/m4.c` / `src/m4.h`) — UNSTABLE

The M4 board is a CPC expansion that adds an SD card and ESP8266 WiFi networking. Our emulation handles:

- **M4ROM signature scan and helper-table publication.** The real M4ROM at `0xC000-0xFFFF` (slot 6) contains pointers at fixed offsets (`0xFF02` = response buffer, `0xFF06` = sock_info table, `0xFF08` = helper-function table). The daemon's `m4crom` routine scans for the `"M4 BOAR\xC4"` signature, then reads these pointers. We host its emulator-side memory in `M4Card::bus_mem` (0xC00 bytes at 0xE800-0xF3FF), `cfg_mem` (256 bytes at 0xF400-0xF4FF), and `sock_mem` (80 bytes at 0xFE00-0xFE4F).

- **Bus bypass.** When the M4ROM slot is the active upper ROM, reads in those three windows return from our internal buffers instead of CPC RAM. There's also a short-budget `ram_mode` (24 reads, cleared per frame by `m4_tick`) that mimics the real board's "RAM mode" — set on small-read network strobes so the SymbOS daemon's banked `m4cred` reader works even with slot 0 selected.

- **File API.** `C_OPEN`, `C_READ`, `C_WRITE`, `C_CLOSE`, `C_SEEK`, `C_GETPATH`, `C_DSKEXT`, `C_READDIR`, `C_FSIZE`, `C_RENAME`, etc. — backed either by a host directory tree (set via `m4_path` in config) or a single raw FAT16/FAT32 image (set via `m4_image`). `src/fat.c` is a minimal in-house FAT reader/writer used in image mode.

- **Networking.** All sockets are backed by host POSIX sockets. `C_NETSOCKET` allocates from a 5-slot table (slot 0 reserved for DNS). `C_NETCONNECT` uses non-blocking `connect()` with up to 5-second polling completion so the daemon sees `status=IDLE` synchronously (matches what `cpc-sdcc`'s SOCKET-poll loop assumes after SDCC hoisted the status read outside the loop). `C_NETRECV` writes the payload starting at `bus_mem[6]` (NOT `[8]` — that's a common misread of the M4 protocol). `C_NETHOSTIP` resolves synchronously via `getaddrinfo` and writes the IP into sock_info[0]. `C_NETRSSI` returns 2 bytes (signal + state); the daemon's UI reads `D` (state) to drive its "Online" / "Unknown error" display.

- **"1984 compatibility shim".** The SymbOS daemon's `m4crcv` performs a `JP <m4cromhrc>` after switching upper-ROM slot to 0, expecting the M4 board's hardware to keep M4ROM accessible at that address. We can't replicate that bus arbitration without breaking unrelated screen-RAM reads. Instead, on every command strobe we patch the in-memory copy of the helper-pointer table at `M4ROM 0xE430` to point at trap stubs we install in `bus_mem` at `0xF300` (hsend) / `0xF310` (hreceive). The stubs are a 5-byte `LD BC, 0xFD3{E,F} ; OUT (C), A`. The `bus_io_write` in `src/cpc.c` catches that OUT, reads the CPU registers (HL=src, DE=dest, length from IYH|C, A=dest bank, IX=return), runs the equivalent bank-aware bulk copy in C, and sets PC = IX.

- **`last_tcp_sock` workaround.** The SymbOS daemon's `m4cscktrn[]` translation table appears to be corrupted between commands in a way we haven't pinned down — see status below. As a pragmatic stop-gap, any TCP command (`CONNECT`/`SEND`/`RECV`/`CLOSE`) whose socket argument doesn't resolve to a valid open TCP socket of ours gets routed to the most-recently-opened TCP socket. Single-connection flows (telnet) limp along with this.

- **Tracing.** `--trace-m4` logs every command's opcode + args + response and every NMI transition to stderr. Useful for diagnosing daemon behaviour.

### Known limitation (M4 + SymbOS daemon)

`netd-m4c.exe` launches, registers, displays "Online", resolves DNS, opens sockets, and connects — but interactive TCP sessions (telnet to telehack.com or aardwolf.org) stall shortly after the initial server banner. Tracing the daemon's runtime memory at the verified `m4cscktrn` location (runtime `0x94A0` in bank `0x1A`, derived by matching the m4ccmd-strobe pattern in the binary and observing `--trace-m4`'s strobe PC) confirms:

1. `m4cscktrn[0] = 0x01` consistently after SOCKET.
2. `m4csct`'s `ld a,(hl)` (PC `0x98C2`) reads `0x01` every time.
3. Yet m4ctrx's immediately-following `ld (m4ctrxcmd+3), a` (PC `0x9702-9705`) writes varying junk (`0x54`, `0x91`, `0xF1`, …).

IRQs do fire in the m4csct/m4ctrx critical path. Best hypotheses: SymbOS's ISR mishandles A across the m4csct→m4ctrx sequence, or our Z80 IRQ servicing has a subtle issue with SymbOS's specific register-save/restore expectations. Without a real-M4 hardware reference trace, the next path forward is to assemble the daemon ourselves to get exact symbol+IRQ-vector locations — which requires Prodatron's internal SymbOS source tree (not in the public SDK).

### Trap port summary

| Port | Direction | Purpose |
|------|-----------|---------|
| `0xFE00`-`0xFFxx` | Write | Accumulate command bytes into `cmd_buf` |
| `0xFC00` | Write | Strobe: dispatch the buffered command |
| `0xFD3E` | Write | Helper hsend trap — runs C-side bulk copy app→M4 |
| `0xFD3F` | Write | Helper hreceive trap — runs C-side bulk copy M4→app |

### Files

- `src/m4.c` / `src/m4.h` — command dispatcher, socket state, helper shim install
- `src/fat.c` / `src/fat.h` — minimal FAT16/FAT32 image accessor
- `src/symbnet.c` / `src/symbnet.h` — synthetic SymbOS network port at `0xFD30/0xFD31`; routes guest-side bytes through the M4 dispatcher. Companion daemon source in `~/Dev/symsys-networkdaemon-1984/` (paused).

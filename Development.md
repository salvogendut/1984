# 1984 CPC Emulator — Development Notes

Architecture and implementation details for contributors.
For usage see [USAGE.md](USAGE.md); for installation see [INSTALL.md](INSTALL.md).

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
| `src/usifac.c` / `usifac.h` | USIfAC II RS232 serial interface — ports 0xFBD0..0xFBDF, PTY or TCP host backend polled once per frame, split RX/TX LED |
| `src/tape.c` / `tape.h` | Cassette / .cdt (TZX) tape decoder — drives PPI Port B bit 7, motor from PPI Port C bit 4, audio mixed into PSG output |
| `src/cpc.c` / `cpc.h` | Top-level machine — bus wiring, frame execution, pixel rendering, reset |
| `src/config.c` / `config.h` | INI config file — load/save `~/.config/1984/1984.conf`, first-run creation, model defaults |
| `src/overlay.c` / `overlay.h` | SDL3 in-app options overlay — tabbed menu, dirty tracking, save-on-close prompt |
| `src/paste.c` / `paste.h` | Host-to-emulator paste — queues clipboard text and injects keypresses into the CPC matrix |
| `src/monitor.c` / `monitor.h` | Memory monitor / debugger — 80×25 terminal window, commands, PTY interface |
| `src/z80dis.c` / `z80dis.h` | Z80 disassembler — standalone, no external dependencies |
| `src/joy.c` / `joy.h` | Joystick/gamepad input — SDL gamepad + raw joystick fallback, hot-plug |
| `src/pilot.c` / `pilot.h` | `--pilot` auto-pilot — host PTY driving the mouse pointer or CPC joystick from polar-coordinate commands (debug/automation harness) |
| `src/main.c` | Entry point — SDL init, event loop, F4–F12 / Ctrl+V / Ctrl± window-scale handling, in-window footer draw |
| `src/host_mount.c` / `host_mount.h` | F10 toggle: pauses the guest and mounts every active FAT card image (M4 SD / IDE / Albireo) on the host so the user can drag files in / out. Backend cascade per card: `gnome-disk-image-mounter` → bare `udisksctl loop-setup`+`mount` → libguestfs `guestmount`. udisks mounts land in `/run/media/$USER/<label>/` as first-class GNOME removable volumes; the guestmount fallback uses `~/.cache/1984/mounts/`. Press F10 again, or eject the card from the file manager (polled via `findmnt` once per frame), to unmount. The main loop then sets `overlay.needs_cold_boot` so the guest's stale FAT cache is dropped on resume. Linux-only; non-Linux stubs return false from `host_mount_open`. Issue #142. |

---

## Render pipeline

The frame render is split into two phases to allow the overlay to composite on top of the CPC video output:

1. **`cpc_frame()`** — runs the CPU and CRTC for one PAL frame (80,000 cycles), writes pixels into `display.pixels[]`, then renders 882 audio samples from the PSG and pushes them to the SDL audio stream
2. **`display_upload()`** — uploads the pixel buffer to the SDL texture, clears the renderer, and blits the texture letterboxed into the window
3. **`overlay_render()`** — draws the overlay on top of the renderer (if visible), using `SDL_SetRenderScale` at 1.5× for the bitmap font
4. **In-window footer** (`src/main.c`) — draws a dark strip just above the LED bar with the model name in bold red and the F-key hint list in light grey, centred horizontally. Uses `SDL_RenderDebugText` at native pixel size (no scale-up; mirrors the bottom-left DBG/fps counter pattern). The OS window title is just `1984` so the footer is the single source of truth for what model is running and which F-keys do what.
5. **`display_flip()`** — calls `SDL_RenderPresent`

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

Mode changes are honoured mid-line, and the CRTC supports overscan and hybrid screens (split-screen via mid-frame R12/R13 reprogramming, R9 character-height changes, and rupture-style raster tricks), so demos that reprogram the CRTC per scanline render correctly.

**Display post-process.** Beyond the raw upload, `display.c` offers optional cosmetic passes, all driven from the Advanced overlay and persisted in `[display]`:

- **Real CRT** (`real_crt`) — a lightweight post-process with adjustable scanline opacity (`crt_scanlines` 0..95), texture brightness (`crt_brightness` 50..100), contrast (`crt_contrast` 50..150), and per-channel red/green/blue gain (`crt_red` / `crt_green` / `crt_blue`, each 50..150) for white-point / tint correction.
- **Monochrome** (`monochrome` = off / green / amber / white) — maps the resolved CPC palette through a Rec. 601 luma transform at ink-resolve time to emulate a period GT65 green-phosphor monitor and its amber / paper-white siblings; the border tints with the inks.
- **Scaling** — integer scale 1×–4× (`scale`, Ctrl ± at runtime) with `fullscreen_smoothing` selecting linear (smooth) vs nearest (sharp) texture filtering.

---

## Memory map

```
0x0000–0x3FFF   OS ROM (lower) or RAM
0x4000–0x7FFF   RAM
0x8000–0xBFFF   RAM
0xC000–0xFFFF   Upper ROM (slot selected by port 0xDFxx) or RAM bank
```

RAM is configurable from 64 KB (464/664 default) up to 576 KB (DK'tronics ceiling) via the options overlay. The physical array in `Mem` is always 576 KB; `Mem.ram_size` (set from `config.memory_kb * 1024`) controls how much of it is accessible. Accesses beyond `ram_size` return 0xFF and writes are silently dropped.

**AMSDOS-headed ROMs.** All ROM loaders (`mem_load_os`, `mem_load_rom`, `mem_load_amsdos`, `mem_load_rom_ext`) go through a `read_rom_image()` helper. If the file is exactly `ROM_BASIC_SIZE + 128` bytes (16512), the 128-byte AMSDOS header is skipped and only the 16384-byte ROM body is loaded. This handles ROMs distributed in DSK-extracted form (e.g. UNIDOS, UNITOOLS, Albireo) transparently — no per-device unwrapping needed.

**Banking.** When the Gate Array receives a byte with bits[7:6] = `11` (port 0x7Fxx), it is a RAM banking command. `Mem.ram_bank` stores bits[5:0] of that byte. All three models honour this command; on real hardware only the 6128 supports it, but the emulator enables banking on the 464 and 664 as well when RAM is expanded beyond 64 KB.

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
| A14=0, A9=0, A8=0 | CRTC select | 0xBCxx |
| A14=0, A9=0, A8=1 | CRTC write | 0xBDxx |
| A14=0, A9=1, A8=0 | CRTC status read | 0xBExx |
| A14=0, A9=1, A8=1 | CRTC data read | 0xBFxx |
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

The AY-3-8912 is clocked at 1 MHz (CPU clock ÷ 4). `psg_render()` is called once per frame and generates 882 stereo frames (44 100 Hz ÷ 50 Hz) of interleaved S16, pushed to an SDL3 audio stream opened with 2 channels.

**Oversampling.** `psg_tick()` advances all three generators by one AY clock. `psg_render()` calls it ~22–23 times per output sample (the exact count varies with the fractional accumulator `clock_rem`) and averages the results. This acts as a box-filter anti-alias on the square waves.

**Fractional clock accumulator.** `clk_per_sample = 1 000 000 / 44 100 = 22.676…`. A `float clock_rem` carries the fraction across samples so pitch is exact rather than drifting ~3% from integer truncation.

**AY ÷8 prescaler.** The real AY chip has an internal ÷8 prescaler before its tone, noise, and envelope counters. All counter thresholds are therefore multiplied by 8 in `psg_tick()`, so that register values from real CPC software work without adjustment (e.g. `SOUND 1,142,…` plays A4 at 440 Hz, not 3 octaves too high). The formula for tone frequency is: `f = 1 MHz / (16 × N)`.

**Envelope.** 32 steps per cycle; each step advances every `ep × 8` AY clocks (where `ep = (R12 << 8) | R11`). Shape bits CONT/ALT/HOLD (R13 bits 3/1/0) select between single-shot, sawtooth, and triangle modes. Writing R13 resets the envelope. `env_counter` is `u32` because `ep × 8` can reach 524 280.

**Low-pass filter.** A one-pole IIR (`lp = (x + lp) >> 1`) is applied after oversampling. This gives a ~7 kHz cutoff at 44 100 Hz, removing the high-frequency aliasing that makes CPC square waves sound metallic.

**Stereo ABC panning.** The three channels are panned to the stereo field following Caprice32's `Index_AL/AR/BL/BR/CL/CR` layout — channel A mostly-left, B centre, C mostly-right. Per-channel L/R amplitude tables are built once (not per-sample); the `audio_stereo_sep` knob (0..255) blends linearly between pure mono (0, the default) and full Caprice32 separation (255). The SDL device always opens 2-channel; the knob only changes the level tables.

**Volume.** `audio_volume` (0..100, default 80) is folded into the same amplitude tables at build time via a perceptual `(vol/100)²` curve — zero per-sample cost.

**DC blocker.** A one-pole high-pass (`R ≈ 0.995`, ~35 Hz corner) runs on the mixed output, replacing the old `(mix*2) − active_scale` centring trick that clicked whenever register 7 toggled a channel mid-frame.

`psg_init()` / `psg_reset()` are split so a warm reset preserves the user's volume and stereo settings; `psg_set_volume()` / `psg_set_stereo()` rebuild the tables live from the overlay.

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

**`memory_kb`** accepts 64, 128, 256, 512, or 576. The 464 and 664 default is 64; the 6128 default is 128. Values outside this set are rejected and the previous value is kept. The field is written directly to `Mem.ram_size` (in bytes) at startup and on every cold boot.

**CLI ROM slot overrides.** `--rom-slot=N:PATH` (repeatable) loads a ROM into slot N after the config-based expansion ROMs are applied. This lets you test or launch with a specific ROM without modifying `1984.conf`. CLI overrides win over config-file assignments for the same slot.

---

## CLI options

`./1984 --help` prints the full list. Below is the same set grouped by purpose so it's easier to scan when you need a specific behaviour. CLI flags ALWAYS win over the persisted config when both are set; `config_save` still targets the default path even when `--config=PATH` was used (i.e. CLI config loads are read-only by design).

### Machine config (override `1984.conf` for this session)

| Flag | Effect |
|------|--------|
| `--464` / `--664` / `--6128` | Force the CPC model |
| `--dd1` | Enable the DDI-1 floppy interface (464 only) |
| `--memory=KB` | RAM size: 64, 128, 256, 512, 576 (also 768 / 1024 via overlay) |
| `--config=PATH` | Read from PATH instead of `~/.config/1984/1984.conf` |
| `--rom-os=PATH` | Replace the OS (lower) ROM |
| `--rom-slot=N:PATH` | Load a ROM into upper-ROM slot N (0–31). Repeatable |

### Media

| Flag | Effect |
|------|--------|
| `--disk-a=PATH` / `--disk-b=PATH` | Mount a DSK image in drive A/B |
| `--load-sna=PATH` | Load a .sna snapshot (v1–v3) after init |
| `--tap=DEVNAME` | Bind Net4CPC to a Linux TAP device for real LAN access. `--tap=` (empty) lets the kernel auto-name (`tap0`…). Needs `CAP_NET_ADMIN` or a pre-created persistent TAP — see the Net4CPC section |

### Boot automation

| Flag | Effect |
|------|--------|
| `--autostart=NAME` | After boot, types `run"NAME` into BASIC |
| `--paste=TEXT` | After boot, types TEXT verbatim (`\n` becomes Enter). Used for end-to-end tests (e.g. `--paste='\|sym'` autoboots SymbOS through M4ROM) |

### Headless test harness

| Flag | Effect |
|------|--------|
| `--screenshot-at=N:PATH` | Save a `.ppm` screenshot at frame N, then exit. The one-shot variant of F4 — drives most QA scripts |
| `--save-sna-at=N:PATH` | Save a .sna snapshot at frame N. Typically pairs with `--paste` / `--autostart` to capture a known boot state |
| `--save-sna-at-ide=N:PATH` | Save a .sna at the Nth ATA command. Used for cross-emulator bisection of CP/M+ / HDCPM boot races |
| `--monitor-pty` | Open a PTY for the F8 memory monitor (connect with `minicom -b 9600 -D <path>` printed to stderr) |
| `--kbd-pty` | Open a PTY that injects writes as keystrokes and streams the firmware text-out (`&BB5A`) — for external test harnesses driving BASIC / CP/M sessions |
| `--ocr-monitor` | Adds an in-memory screen-text reader: scans video RAM each frame, decodes against the firmware font, streams the 80×25 (or 40×25) char grid on change out the kbd PTY. Lets probes follow CP/M+ output that bypasses `&BB5A`. Implies `--kbd-pty` |
| `--pilot[=ARG]` | Open a PTY that auto-pilots the mouse pointer (or CPC joystick) from host-sent polar commands (`R THETA`, `press N`, `target mouse\|joy`). `ARG` = a symlink path for a stable alias, or `mouse`/`joystick` to pick the initial target. See [docs/PILOT.md](docs/PILOT.md) |

### Tracing (stderr)

All of these are off by default. They are independent (combine freely).

| Flag | What it logs |
|------|--------------|
| `--trace-io` | CRTC + Gate Array register writes |
| `--trace-palette` | Palette writes + the firmware-flush fallback |
| `--trace-input` | Keyboard + joystick events (row-9 scans, key up/down, gamepad/joystick events) |
| `--trace-m4` | Every M4 board command opcode + args + response, plus NMI transitions |
| `--trace-symbos-msg` | SymbOS RST #10 message sends in the net-daemon range. See M4 section for `ONE_K_TRACE_SYMBOS_ALL` |
| `--trace-albireo` | Every Albireo / CH376 command + interrupt status |
| `--trace-net4cpc` | W5100S register access + socket commands + TX/RX bursts |
| `--trace-tap` | TAP-backed network stack events (ARP, IP, UDP, ICMP, TCP) |

### Debug env vars (not CLI flags)

These are read via `dbg_getenv()` so they only fire when the master `cfg.debug` is on (toggle in Overlay → Advanced → Debugging). They print to stderr like the `--trace-*` flags.

| Env var | What it logs |
|---------|--------------|
| `ONE_K_TRACE_AUDIO` | SDL audio open/resume errors, periodic queued-bytes + min/max sample stats |
| `ONE_K_TRACE_PSG` | Every AY register select and write with PC + I/O port |
| `ONE_K_TRACE_M4_IO` | Every M4 ACKPORT strobe with the cmd_buf head (16 bytes) and CPU state |
| `ONE_K_TRACE_SYMBOS_ALL` | Widens `--trace-symbos-msg` from "net-daemon messages only" to every RST #10 (very chatty — for cross-daemon startup bugs) |
| `ONE_K_TRACE_HDCPM` | Upper-ROM-select trace into the HDCPM ROM (legacy from #102) |
| `ONE_K_TRACE_BDF4` | Firmware tick-vector and reset-detection trace (legacy from #102 / #129) |
| `ONE_K_RESET_SNA=PATH` | When the firmware reset is detected (via the `$BDF4` trigger), dump a snapshot to PATH |

---

## Options overlay

The overlay (`src/overlay.c`) is a lightweight immediate-mode UI rendered with `SDL_RenderDebugText` at 1.5× scale. It has four tabs:

| Tab | Rows |
|-----|------|
| General | Model, Memory, MX4, Roms Board, OS ROM, BASIC ROM, External Tape (6128) |
| Media | Drive A, Drive B, Tape |
| Extensions | M4, UliFAC [unimplemented], Net4CPC, RTC, DD1, ROM Slots →, Diag Cart, SYMBiFACE IDE, SYMBiFACE Mouse, Albireo, Cyboard |
| Advanced | Smoothing, Real CRT (+ Scanlines / Brightness / Contrast / R / G / B sub-rows when on), Load/Save Snapshot, Net4CPC TAP, DHCP server, Debugging, Capture video, USIfAC mode, USIfAC PTY link, Monochrome, Printer mode, Volume, Stereo, Notifications, Version |

The **Advanced** tab (`OV_TINKER`) is hidden unless `cfg.tinker` is enabled (General → Tinker). Most of its rows cycle a value on Enter (Volume steps 5%; Stereo cycles mono → half → full; Notifications cycles screen → console → off; Monochrome cycles off → green → amber → white) and apply live without a cold boot; a few open file pickers (snapshots, video capture) or an inline text editor (USIfAC PTY link). The CRT rows (Scanlines / Brightness / Contrast / R / G / B) only appear while Real CRT is on, via a `tinker_logical_row()` remap.

The overlay snapshots the Config struct on open. If the user changes any value and then closes (ESC or F9), a "Save changes?" dialog appears. Enter saves to disk; ESC reverts to the snapshot. Switching the model automatically updates RAM size and ROM paths via `config_set_model()`.

The **Memory** row (General tab, row 1) cycles through 64 → 128 → 256 → 512 → 576 → 768 → 1024 KB on Enter for all three models. A memory change sets `dirty = true` and, on save, adds `memory_kb != saved.memory_kb` to the cold boot trigger so `main.c` updates `Mem.ram_size` before calling `cpc_reset()`.

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

## Notifications (`src/notify.c`)

A global singleton sink for short informational messages (USIfAC link-up, client connect/disconnect, etc.). Any translation unit posts with `notify_post(fmt, …)` — no context handle needed. Three modes (`notifications` in `[advanced]`, cycled live from the overlay):

- **screen** (default) — a fading "smoked" toast panel anchored bottom-left, just above the LED/hint bar. A 5-slot ring with a 3.5 s TTL and a 0.6 s alpha fade-out; newest sits lowest. Rendered each frame by `notify_render()` in the overlay's 1.5× logical space, called just before the buffer flip.
- **console** — `fprintf(stderr)` only (the legacy behaviour).
- **off** — silent.

In **screen** mode the message is *also* echoed to stderr when the debug master switch (`g_debug_enabled`) is on, so the log trail survives past the transient panel without forcing the user into console mode. `notify_init()` / `notify_set_mode()` are called after config load in `main.c`; `notify_tick(20)` advances ages once per displayed frame.

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

**TAP backend (Linux).** When `cfg.net4cpc_tap=true`, the W5100S is bound to a kernel TAP device instead of POSIX sockets. Outbound TX is assembled as real Ethernet + IP + UDP/TCP frames in `src/n4c_stack.c`, ARP / ICMP / TCP are handled in-process, and inbound frames are demultiplexed back to the matching W5100S socket. The chip is then a true L2 endpoint — pingable from the LAN, accepts inbound connections, DHCP works.

`main.c::net4cpc_tap_sync()` is idempotent and runs at boot plus after any overlay save. On first enable it auto-provisions the tap via a single `pkexec` call (tuntap add, link up, addr add, firewalld trusted, sysctl ip_forward, narrow MASQUERADE + FORWARD iptables rules tagged `1984-<dev>`). The device is persistent for the host uptime — reused across launches, only recreated when the user changes the configured subnet (`net4cpc_tap_host_ip` / `_netmask`).

A minimal **DHCPv4 server** lives in `n4c_stack.c::dhcp_try_handle()`, hooked from the chip's outbound `send_udp` path: DISCOVER → OFFER, REQUEST → ACK, single in-range lease per MAC. Server-side host_ip, netmask, and lease range are configurable. A small synchronous **DNS proxy** in the same file forwards chip queries on port 53 to the host's `/etc/resolv.conf` upstream (skipping `127.0.0.0/8` and the proxied gateway to avoid loops).

**W5100S correctness fixes** required for KCNet/SymbOS DHCP to actually work: `Sn_IR` is write-1-to-clear per datasheet §4.2.10; soft reset (`MR.RST`) preserves the common-config block (`0x0000`–`0x002F`); `PHYCFGR` reads `0x07` (LNK | SPD | DPX); the indirect-bus `0x8000` mask is applied. Without these the bring-up showed `DHCPDECLINE` after `ACK`, BASIC reset on `ncfg -r`, and `network-cable not connected` from KCNet utilities.

**Legacy POSIX-socket backend** (TAP off, or non-Linux): outbound TCP/UDP work, but no DHCP — the host kernel silently drops broadcast OFFERs that aren't addressed to it. Use a static IP for N4C consumers in that mode, or switch to TAP.

**Tracing.** `--trace-net4cpc` enables `net4cpc_trace = 1`. Every register access is logged with its decoded name (`MR`, `SHAR`, `SIPR`, `Sn_MR`, `Sn_DIPR`, etc.) and socket index where applicable. Socket commands are logged with mnemonic (`OPEN`, `CONNECT`, `SEND`, `RECV`, `CLOSE`). TX/RX ring-buffer accesses are summarised as `burst R/W [start..end] N bytes` to avoid spamming one line per byte. The MR shortcut read at `0xFD20` and `MR.RST` events get their own labelled lines.

---

## Cassette tape (`src/tape.c`)

CDT (TZX) decoder, ported in compact form from Caprice32's `tape.cpp`. Drives the PPI Port B bit 7 (cassette data input) from the loaded image while the motor is on.

**Block dispatch.** `tape_load()` reads the whole file into memory; if the first 8 bytes are the `"ZXTape!\x1A"` signature the block stream is taken to start at offset 10 (signature + version word), otherwise at byte 0. `next_block()` walks the stream, dispatching on the block-type byte: `0x10` standard speed data, `0x11` turbo data, `0x12` pure tone, `0x13` pulse sequence, `0x14` pure data, `0x20` pause. Metadata-only blocks (`0x21`, `0x22`, `0x30`–`0x34`, `0x5A`) are skipped past. Unknown block types fall back to a generic skip using the "extension rule" — a 4-byte length immediately after the type byte tells the decoder how many bytes to jump.

**Pulse cycle scaling.** TZX timings are expressed in Spectrum 3.5 MHz T-states. The CPC runs at 4 MHz, so a `CYCLE_SCALE()` macro multiplies by 40/35. A separate `MS_TO_CYCLES()` converts milliseconds (used for pause blocks) into CPU cycles at 4 MHz / kHz.

**State machine.** `update_level()` runs per "tape tick" — the decoder maintains a `cycles_until_next` countdown driven by the Z80 step loop. Each call flips the cassette line level (`switch_level()`) and either re-arms with the next pulse-pair cycle count or advances to the next stage (pilot → sync → data → pause → next block). Data bits are pulled MSB-first from the block payload; each bit is two pulses, the cycle count picked from `zero_pulse_cycles` or `one_pulse_cycles` per the bit value.

**Z80 step integration.** `cpc_frame()` calls `tape_step(&cpc->tape, t)` after every `z80_step` with the instruction's T-state count. `tape_step` subtracts that from `cycles_until_next` and, when it goes non-positive, runs `update_level()`. The motor (PPI Port C bit 4) gates the whole thing — when the motor is off the call is a no-op, and the level holds whatever it was last set to.

**Audio mixing.** While the motor is on, the same step loop also samples the current tape level at audio rate (`cycles * AUDIO_SAMPLE_RATE >= cpu_clk_hz` ≈ 90.7 cycles per sample at 4 MHz / 44.1 kHz) and writes ±2500 into `cpc->tape_audio[]`. After PSG render produces a frame's worth of samples, that buffer is summed into the PSG output with saturating clamp before going to SDL — that's the loading-screech sound players expect.

**Model wiring.** On the 464 the deck is built in, so the tape is always wired when `cfg.tape` is non-empty. The 664 and 6128 have no built-in deck — a `cfg.external_tape` toggle (General → External Tape, row only visible on disk machines) controls whether the cassette is virtually plugged into the tape port. Both the boot path and the cold-boot path in `main.c` consult this when calling `tape_load`. Toggling `external_tape` or changing the tape image triggers a cold boot.

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

## FUZIX (ajcasado `cpcsme` port)

Boots to a multi-user shell on a 6128 + ≥512 KB + SymbIface IDE. Step-by-step recipe and what's known about the platform layer are in [docs/issue-62-fuzix-notes.md](docs/issue-62-fuzix-notes.md).

Two emulator bugs surfaced during bring-up — both root-caused to **port decoding that was too loose** relative to real hardware:

- **`1a3393a` — FDC at `0xFB**` shared the upper half with peripherals.** Real CPC decodes the µPD765 at `0xFB7E/0xFB7F` only; the upper half (`lo bit 7 = 1`) is free for expansion peripherals. We used to claim the entire `0xFB**` range, so FUZIX's Usifac probe at `0xFBD8` read the FDC main status register instead of `0xFF` and the driver hung in `usifac_flush()` polling a chip that wasn't there. Fix: gate the FDC read on `!(lo & 0x80)`.
- **`a3877ee` — CRTC write decode ignored `A9`.** Real CPC CRTC chip-select is `A14=0`; `A9` then selects the four functions (select / write / status read / data read). Our write path checked only `A14` and `A8`, so an `OUT` to any port with `A14=0,A8=1` was treated as a register write — even `A9=1` read ports. FUZIX's SDCC port helper at kernel `F995` issues `OUT (C),B` with `BC=0x03FF` (A14=0, A9=1, A8=1 — the CRTC data-read port) for unrelated bus work; real hardware ignores it, we clobbered `R12 := 0x03`, pointed the screen at block 0 and produced the band-of-pixels-per-text-row corruption first captured in `1984-20260616-153532.gif`. Fix: add `&& !(hi & 0x02)` (A9=0) to the CRTC write-path test.

A third surfaced when enabling **Albireo (CH376)** as the boot device (issue #146):

- **`020e9a1` — CH376 INT# handshake missing.** Our CH376 emulation served UNIDOS, which polls the DATA port for the one-shot reply byte and never reads STATUS. FUZIX's `ch375` driver instead polls the STATUS port for the INT# pin going active after `SET_USB_MODE` and `DISK_INIT`, then issues `GET_STATUS` to read the interrupt code. We never set `int_pending` on those commands and didn't implement `DISK_INIT (0x51)` at all, so the probe printed `ch375: timeout, rstat=80` and gave up. Fix: raise `USB_INT_CONNECT` on host-mode `SET_USB_MODE` (`mode>=0x04`) and add a `DISK_INIT` case that raises `USB_INT_SUCCESS` when an image is mounted. UNIDOS is unaffected — it doesn't poll STATUS.

Lesson worth remembering for the next OS port: **if a guest writes a CPC peripheral port we don't expect to see, suspect over-broad decode before suspecting guest behaviour.** Two debug hooks added during this investigation are kept gated behind `dbg_getenv()` for next time:

- `DUMP_VIDEO_RAM=/path/file` — writes physical RAM + the 18 CRTC registers once per frame (last write wins; quit on the bad frame and the file captures it).
- `ONE_K_TRACE_CRTC_REGS=1` — `[CRTC] PC=… R… = 0x… BC=… HL=… DE=… AF=…` for every register write; the CPU register dump is what isolated the `BC=0x03FF` smoking gun.

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

### Scripted and live injection

Two host-driven paths inject into the same row-9 / mouse plumbing as real input, for headless tests and AI-driven debugging:

- **`--joy-script=SPEC`** (`src/joy.c`) — replays a fixed `DIRS:FRAMES` timeline into row 9. `joyscript_tick()` runs once per frame next to `paste_tick()`. The deterministic, record-once analogue of `--paste`.
- **`--pilot[=ARG]`** (`src/pilot.c`) — opens a host PTY and reacts *live* to polar-coordinate commands, holding the last velocity until changed. Routes to the **mouse** (`mouse_*` + `ch376_mouse_*`, gated on `cpc.symbiface_mouse` / `cpc.albireo_mouse`) or the **CPC joystick** (row 9, 8-way + Fire1/2), switchable at runtime with `target mouse|joy`. PTY setup mirrors `usifac.c`; `pilot_tick()` runs once per frame. Full protocol in [docs/PILOT.md](docs/PILOT.md).

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

## M4 board (`src/m4.c` / `src/m4.h`)

The M4 board is a CPC expansion that adds an SD card and ESP8266 WiFi networking. Our emulation handles:

- **M4ROM signature scan and helper-table publication.** The real M4ROM at `0xC000-0xFFFF` (slot 6) contains pointers at fixed offsets (`0xFF02` = response buffer, `0xFF06` = sock_info table, `0xFF08` = helper-function table). The daemon's `m4crom` routine scans for the `"M4 BOAR\xC4"` signature, then reads these pointers. We host its emulator-side memory in `M4Card::bus_mem` (0xC00 bytes at 0xE800-0xF3FF), `cfg_mem` (256 bytes at 0xF400-0xF4FF), and `sock_mem` (80 bytes at 0xFE00-0xFE4F).

- **Bus bypass.** When the M4ROM slot is the active upper ROM, reads in those three windows return from our internal buffers instead of CPC RAM. The real SymbOS daemon banks M4ROM in for its response copies, so we do not need a broader "RAM mode" overlay outside that.

- **File API.** `C_OPEN`, `C_READ`, `C_WRITE`, `C_CLOSE`, `C_SEEK`, `C_GETPATH`, `C_DSKEXT`, `C_READDIR`, `C_FSIZE`, `C_RENAME`, etc. — backed either by a host directory tree (set via `m4_path` in config) or a single raw FAT16/FAT32 image (set via `m4_image`). `src/fat.c` is a minimal in-house FAT reader/writer used in image mode. `C_CLOSE` returns `BADFD` for unknown fds including the AMSDOS-compatible fixed handles 1 and 2 — M4ROM's boot sequence closes those at startup and uses BADFD as a flow gate for the banner / autoboot path. Returning OK there breaks boot.

- **Command parsing.** Packets are parsed at `cmd_buf` offset 0 with `cmd_len` as total length. `cmd_buf[0]` is the count of header bytes after itself (per the daemon's `m4ccmd0`), NOT total packet length — for variable-payload commands like `C_NETSEND` the payload is written after the header via a separate fast-block transfer (`m4csnd`) before the ACK strobe, so `cmd_len` at ACK time = header + payload. A defensive fallback scans `cmd_buf` for the 0x43 sentinel if offset 0 doesn't look like a valid header.

- **Networking.** All sockets are backed by host POSIX sockets. `C_NETSOCKET` allocates from a 5-slot table (slot 0 reserved for DNS). `C_NETCONNECT` uses non-blocking `connect()`; the socket stays in `status=1` (connecting) for one `m4_tick` after a synchronous `r==0` so the SymbOS daemon's `nettcp` observes the `1→0` edge that triggers `MSR_NET_TCPEVT`. `C_NETRECV` writes the payload starting at `bus_mem[6]` (NOT `[8]` — that's a common misread of the M4 protocol). `C_NETHOSTIP` resolves synchronously via `getaddrinfo` and writes the IP into sock_info[0]. `C_NETRSSI` returns 2 bytes (signal + state); the daemon's UI reads `D` (state) to drive its "Online" / "Unknown error" display. `C_GETNETWORK` returns the full 196-byte structure including the synthetic MAC at offsets 190..195.

- **"1984 compatibility shim".** The SymbOS daemon's `m4crcv` performs a `JP <m4cromhrc>` after switching upper-ROM slot to 0, expecting the M4 board's hardware to keep M4ROM accessible at that address. We can't replicate that bus arbitration without breaking unrelated screen-RAM reads. Instead, on every command strobe we patch the in-memory copy of the helper-pointer table at `M4ROM 0xE430` to point at trap stubs we install in `bus_mem` at `0xF300` (hsend) / `0xF310` (hreceive). The stubs are a 5-byte `LD BC, 0xFD3{E,F} ; OUT (C), A`. The `bus_io_write` in `src/cpc.c` catches that OUT, reads the CPU registers (HL=src, DE=dest, length from IYH|C, A=dest bank, IX=return), runs the equivalent bank-aware bulk copy in C, and sets PC = IX.

- **`last_tcp_sock` workaround.** Single-connection guest code occasionally hits `m4csct` with a daemon-socket index that doesn't translate to a live host socket. As a stop-gap, any TCP command (`CONNECT`/`SEND`/`RECV`/`CLOSE`) whose socket argument doesn't resolve to a valid open TCP socket gets routed to the most-recently-opened TCP socket. Single-connection flows (settime, symtel, wget) work cleanly with this.

- **Tracing.** `--trace-m4` logs every command's opcode + args + response and every NMI transition to stderr. For SymbOS-side debugging, `--trace-symbos-msg` hooks Z80 RST #10 (the SymbOS message-send vector) and logs net-daemon-class messages — useful for confirming whether the daemon ever emits `MSR_NET_TCPEVT` (= 0x9F) or the client ever issues `FNC_NET_TCPSND` (= 0x14) without having to instrument the daemon binary itself. Set `ONE_K_TRACE_SYMBOS_ALL=1` to widen the filter to every RST #10.

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
- `src/symbos_trace.c` / `src/symbos_trace.h` — `--trace-symbos-msg` RST #10 message hook. The hook is NULL by default (zero hot-path overhead) and is only installed when the flag is given.

## Building on Haiku

1984 builds and runs on Haiku (tested on the 32-bit nightly). Three environment
quirks to be aware of:

1. **Use the modern GCC, not the legacy BeOS-compat one.** Haiku ships both
   `gcc2` (binary-compat with BeOS R5) and a modern GCC 13 side by side. The
   default `cc` may point to `gcc2`, which does not support C11. Switch shells
   with `setarch x86` (or `setarch x86_64` on 64-bit Haiku) before running
   `autoreconf`/`configure`/`make`.

2. **Secondary-arch pkg-config path.** On 32-bit Haiku the SDL3 package is
   `libsdl3_x86` / `libsdl3_x86_devel`, and its `sdl3.pc` lives under
   `/boot/system/develop/lib/x86/pkgconfig/`, which `pkg-config` does not search
   by default. `configure.ac` detects `host_os = haiku*` and prepends that
   directory to `PKG_CONFIG_PATH` automatically.

3. **Sockets are in `libnetwork`, not libc.** Handled automatically by the
   `AC_SEARCH_LIBS([socket], [network socket])` probes in `configure.ac`; no
   manual `LDFLAGS` needed.

## Building on NetBSD (pkgsrc)

1984 builds and runs on NetBSD with no source-level changes. Only two
environment quirks:

1. **`aclocal` does not search pkgsrc paths by default.** pkg-config's
   `pkg.m4` (and any other macros installed via pkgsrc) live under
   `/usr/pkg/share/aclocal/`, which is outside `aclocal`'s built-in
   search path. Without it, `PKG_CHECK_MODULES` fails to expand and
   `autoreconf` complains about *unrelated* macros being undefined
   (`AC_MSG_ERROR` is the usual misleading scapegoat). Export
   `ACLOCAL_PATH=/usr/pkg/share/aclocal` before running `autoreconf`.

2. **Use `gmake`, not BSD `make`.** automake emits GNU-make-flavoured
   rules. The pkgsrc package is `gmake`; it does not replace
   `/usr/bin/make`.

Required pkgsrc packages: `pkgconf autoconf automake gmake SDL3`.

## Building on Windows (MinGW-w64)

1984 builds and runs on Windows via the MSYS2 MinGW64 toolchain (verified
under Wine on Linux). Four points worth knowing:

1. **Socket library.** Winsock lives in `ws2_32`, not libc.
   `configure.ac` detects `host_os = mingw*|cygwin*|msys*` and appends
   `-lws2_32` via the `WIN_LIBS` substitution. The Haiku/Solaris
   `AC_SEARCH_LIBS` probes are skipped on Windows for the same reason —
   no `socket`/`getsockopt` symbol exists outside `ws2_32`.

2. **Console subsystem.** SDL3's `sdl3.pc` adds `-mwindows` to the link
   line, which marks the binary as a GUI app and silently swallows
   `stderr`/`stdout` from `cmd.exe`. `configure.ac` appends `-mconsole`
   *after* the pkg-config flags (`WIN_SUBSYSTEM` in `Makefile.am`) so
   trace output and error messages remain visible. Cosmetically this
   means launching `1984.exe` from Explorer flashes a console window;
   that is acceptable for an emulator with developer-facing trace flags.

3. **`CPC` instance lives in BSS, not on the stack.** `main()` declares
   the CPC struct `static` rather than as an automatic. The struct
   embeds 1 MB of guest RAM plus ROMs and buffers, which exceeds the
   default 1 MB Windows thread stack. MinGW's GCC inserts `__chkstk_ms`
   probes for any frame over 4 KB; on Windows those probes hit the
   stack guard page and raise `EXCEPTION_STACK_OVERFLOW` before `main`
   even reaches its first statement. Linux (8 MB stack with on-demand
   growth) never noticed.

4. **Compat shims.** `src/compat_win.h` provides small adapters for
   POSIX APIs that MinGW does not ship verbatim: a single-arg
   `mkdir(path)` macro (Windows `_mkdir` ignores mode bits), and PTY
   stubs in `monitor.c` so `--monitor-pty` returns a clean "not
   supported on Windows" message instead of failing to link. Socket
   code in `m4.c`, `net4cpc.c`, and `monitor.c` casts buffer/optval
   arguments to `char *` for Winsock's stricter prototypes.

## Continuous integration

`.github/workflows/build.yml` defines the build, package, and release jobs.
Linux and Windows run on every push to `main`/`windows-port` and every PR to
`main`; the Flatpak job runs on `main`, tags, and manual dispatch (not PRs).

- **Linux** — runs inside a `fedora:41` container, installs the standard
  build deps (`gcc make autoconf automake pkgconf SDL3-devel`), runs
  `autoreconf -iv && ./configure && make`, uploads the `1984` ELF as the
  `1984-linux-x86_64` artifact.
- **Windows** — uses `msys2/setup-msys2@v2` with `MINGW64`, installs the
  same toolchain plus `mingw-w64-x86_64-sdl3`, builds the same way, then
  bundles `1984.exe` with the DLLs reported by `ldd` and the ROM set
  into the `1984-windows-x86_64` artifact. `cache: true` keeps the
  pacman package cache between runs so subsequent setups drop from ~8
  minutes to ~30 seconds.
- **Flatpak** — builds `io.github.salvogendut.Emulator1984.yml` in the
  `freedesktop-24.08` container via the `flatpak/flatpak-github-actions`
  action and uploads the `1984.flatpak` bundle. Skipped on PRs because the
  manifest builds `branch: main` (the Linux job already covers PR compile
  breakage). See [docs/FLATPAK.md](docs/FLATPAK.md).

On a `v*` tag the **release** job (`needs: [linux, windows, flatpak]`)
packages all three artifacts and publishes them to a GitHub Release —
`1984-<tag>-linux-x86_64`, `1984-<tag>-windows-x86_64.zip`, and
`1984-<tag>-x86_64.flatpak`.

# 1984 — Amstrad CPC Emulator

![1984](1984.png)

A cycle-stepped Amstrad CPC 464/6128 emulator written in C with SDL3.

## Status

Boots to Locomotive BASIC. Keyboard, disk (DSK images via µPD765 FDC), AMSDOS file loading, audio (AY-3-8912 / PSG with tone, noise, envelope), and joystick/gamepad (USB, Bluetooth, hot-plug) work. Commercial games and standard software run well. Software, like demos, that relies on undocumented hardware behaviour or cycle-exact CRTC tricks is untested and may not work correctly.

## Requirements

- GCC (C11)
- SDL3
- CPC ROM images (not included — dump your own or source from the web)

## Build

Two build systems are provided. Use whichever suits your platform.

**GNU Make (Linux, quick iteration):**
```bash
make
```
Output binary: `bin/1984`

**Autoconf/Automake (cross-platform — Linux, macOS, FreeBSD, OpenBSD, …):**
```bash
./configure
make
```
Output binary: `./1984`

If `configure` is missing (e.g. after a fresh clone without the generated files), regenerate it with:
```bash
autoreconf -fiv
```
This requires `autoconf` and `automake`. On most systems they are available as packages (`autoconf`, `automake`).

**RPM package (Fedora / RHEL / CentOS):**

A `1984.spec` file is included. Build an RPM with:
```bash
rpmbuild -ba 1984.spec
```
The spec file handles `autoreconf`, `./configure`, and `make install` automatically.

## ROM files

Place ROM images in the `roms/` directory with these exact names:

| File | Contents |
|------|----------|
| `roms/OS_464.ROM` | CPC 464 OS ROM (16 KB) |
| `roms/BASIC_1.0.ROM` | CPC 464 Locomotive BASIC 1.0 (16 KB) |
| `roms/OS_6128.ROM` | CPC 6128 OS ROM (16 KB) |
| `roms/BASIC_1.1.ROM` | CPC 6128 Locomotive BASIC 1.1 (16 KB) |
| `roms/AMSDOS.ROM` | AMSDOS disk filing system (16 KB) — required for disk access on 6128; optional on 464 (needs DD1) |
| `roms/AmstradDiagLower.rom` | Amstrad Diagnostics lower ROM (optional — enables Diag Cart toggle in the overlay) |

## Configuration

On first run a configuration file is created at `~/.config/1984/1984.conf`. You can edit it directly or use the in-app options overlay (F9).

```ini
[machine]
model=6128        # 464 or 6128
memory=128        # 64, 128, 256, 512, or 576 (KB); default 64 for 464, 128 for 6128

[roms]
os=~/.config/1984/roms/OS_6128.ROM
basic=~/.config/1984/roms/BASIC_1.1.ROM
amsdos=~/.config/1984/roms/AMSDOS.ROM   # 6128 only; cleared automatically for 464

[expansion_roms]
# Load extra ROMs into upper ROM slots 0-31.
# Slot 0 = BASIC fallback, slot 7 = AMSDOS fallback; all slots can be overridden.
# Example: slot_5=~/.config/1984/roms/TOOLKIT.ROM

[hardware]
dd1=false         # CPC 464 only — DDI-1 floppy interface (enables drives + AMSDOS in slot 7)
m4=false
ulifac=false
net4cpc=false

[display]
scale=2           # 1, 2, or 3
fullscreen=false
```

## Usage

```bash
./bin/1984 [OPTIONS]
```

| Option | Description |
|--------|-------------|
| `--464` | Boot as CPC 464 (overrides config) |
| `--6128` | Boot as CPC 6128 (overrides config) |
| `--dd1` | Enable DDI-1 floppy interface on CPC 464 (overrides config) |
| `--disk-a=PATH` | Mount a DSK image in drive A (overrides config) |
| `--disk-b=PATH` | Mount a DSK image in drive B (overrides config) |
| `--rom-slot=N:PATH` | Load a ROM image into upper ROM slot N (0-31); may be repeated |
| `--rom-os=PATH` | Override the lower ROM (OS) with a custom image at PATH |
| `--autostart=NAME` | After boot, types `run"NAME` into BASIC |
| `--paste=TEXT` | After boot, types TEXT verbatim (`\n` becomes Enter) |
| `--monitor-pty` | Open a PTY for the memory monitor (`minicom -b 9600 -D <path>`) |
| `-h`, `--help` | Print this option summary and exit |

Passing an unrecognised option prints the usage summary to stderr and exits with code 1.

Examples:

```bash
# Boot with a disk mounted in drive A
./bin/1984 --disk-a=game.dsk

# Autostart a specific file from the disk
./bin/1984 --disk-a=game.dsk --autostart=game

# Run a disk-based game that needs its own loader command
./bin/1984 --disk-a=game.dsk --paste='|disc\nrun"disc'

# Load a toolkit ROM into slot 5 at startup
./bin/1984 --rom-slot=5:~/.config/1984/roms/TOOLKIT.ROM

# Multiple ROM slots can be specified
./bin/1984 --rom-slot=5:~/.config/1984/roms/TOOLKIT.ROM --rom-slot=8:~/.config/1984/roms/OTHER.ROM
```

The machine model can be selected from the command line (`--464` / `--6128`) or via the options overlay (F9).

| Key    | Action |
|--------|--------|
| F4     | Save screenshot (`<binary>_<timestamp>.ppm`) and play camera shutter sound |
| F5     | Warm reset |
| F8     | Open/close memory monitor / debugger |
| F9     | Open/close options overlay |
| F11    | Toggle fullscreen |
| F12    | Quit |
| Ctrl+V | Paste clipboard text into the emulator |

### Joystick / gamepad

Any USB or Bluetooth controller recognised by SDL3 is automatically mapped to CPC joystick 1 (keyboard matrix row 9). Hot-plug is supported — controllers can be connected or disconnected at any time.

| Controller input | CPC joystick |
|---|---|
| D-pad or left stick | Up / Down / Left / Right |
| South button (A / Cross) | Fire 1 |
| East / West / North buttons | Fire 2 |

### Paste from host (Ctrl+V)

Pressing Ctrl+V types the host clipboard contents into the emulator one character at a time, simulating keypresses through the CPC keyboard matrix. Useful for entering BASIC programs. Supports letters, digits, common punctuation, and newlines. Each pasted block ends with an automatic Return.

### Options overlay (F9)

The overlay lets you change the machine model, RAM size, ROM paths, and hardware options without editing the config file. Navigate with arrow keys, press Enter to cycle a value. On close, if anything changed you will be asked whether to save.

Switching the model automatically sets the matching ROM paths and RAM size.

**RAM size** (Advanced → Memory): press Enter to cycle through 64, 128, 256, 512, 576, 768, and 1024 KB. Up to 576 KB uses DK'tronics-compatible banking (Gate Array port 0x7Fxx, data bits[5:3] select the 64 KB bank group). 768 KB and 1024 KB switch to the Yarek/RAM7 extended scheme, where port address bits A10–A8 carry an additional bank group selector: port 0x7Exx adds a second 512 KB block (576–1088 KB range), giving a practical ceiling of 1024 KB with bank_high values 0–1. Banking is supported on both the 464 and 6128. Changing RAM size triggers a cold boot on save.

**CPC 464 and DD1:** on the 464, the Storage tab drives are greyed out by default. Enable **DD1** in the Advanced tab to activate the DDI-1 floppy interface — this enables drive access and loads AMSDOS into ROM slot 7. On the 6128, drives are always enabled and the DD1 option is greyed out.

**ROM Slots** (Advanced → ROM Slots) opens a sub-panel listing the lower ROM and all 32 upper ROM slots (0–31):

| Entry | Default | Enter | Delete |
|-------|---------|-------|--------|
| Lower ROM | Model OS ROM | Replace with file picker | Restore model default |
| Slot 0 | BASIC ROM | Load expansion override | Clear override / restore default BASIC |
| Slot 7 | AMSDOS ROM | Load expansion override | Clear override / restore default AMSDOS |
| Slots 1–6, 8–31 | empty | Load ROM into slot | Clear slot |

**Diagnostics Cartridge** (Advanced → Diag Cart): toggles the lower ROM between the model's default OS and `AmstradDiagLower.rom`. When ON, the machine boots into the Amstrad Diagnostics program. When OFF, the lower ROM reverts to the model's normal OS ROM. The toggle is greyed out if `AmstradDiagLower.rom` is not found in the ROMs directory. The change triggers a cold boot on save.

**Net4CPC** (Advanced → Net4CPC): enables emulation of the Net4CPC Ethernet add-on board based on the WIZnet W5100S chip. When enabled, four I/O ports are exposed at 0xFD20–0xFD23:

| Port | Name | Description |
|------|------|-------------|
| 0xFD20 | MR | Mode Register — reads 0x03 when the chip is present |
| 0xFD21 | IDM_ARH | High byte of the 16-bit indirect address register |
| 0xFD22 | IDM_ARL | Low byte of the 16-bit indirect address register |
| 0xFD23 | IDM_DR | Data register — read/write to the W5100S register space at the current address; auto-increments when MR bit 1 (AI) is set |

Socket operations (TCP connect/send/receive, UDP sendto) are backed by host POSIX sockets. Four sockets (0–3) are available, each with 2 KB TX and 2 KB RX ring buffers. This is compatible with the Z80 driver in the [N4C-NETTOOLS](https://github.com/salvogendut/n4c-nettools) library. The toggle triggers a cold boot on save.

Changes to the model, RAM size, DD1 toggle, any ROM slot, or the lower ROM trigger an automatic cold boot so the new configuration takes effect immediately. The machine re-boots without needing to quit and restart.

### Memory monitor / debugger (F8)

Press **F8** to open a separate 80×25 green-phosphor terminal window. All commands
are also available via a PTY serial port with `--monitor-pty`.

| Command | Description |
|---------|-------------|
| `D <addr> [<end>]` | Disassemble Z80 (10 lines default; pageable) |
| `M <addr> [<end>]` | Hex + ASCII dump (page default; ASCII in reverse video) |
| `B [<addr>]` | Set a breakpoint / list all breakpoints |
| `BC <n>` | Clear breakpoint slot n (0 – 15) |
| `N` | Single-step one instruction (when paused) |
| `G` | Resume execution |
| `GA` | Gate Array: screen mode + all 16 inks |
| `CRTC` | All 18 CRTC registers + live counters |
| `X` / `Q` | Close monitor |

When a breakpoint fires the emulator freezes, the monitor opens automatically,
and shows the hit address with a 5-line disassembly. A live register bar
(`PC SP A F BC DE HL IX IY`) is always visible at the bottom of the window,
turning red while paused.

```bash
# Connect to the monitor over serial with minicom
./1984 --monitor-pty
minicom -b 9600 -D /dev/pts/N    # path printed to stderr at startup
```

See [Development.md](Development.md) for the full architecture and implementation notes.

## Development

See [Development.md](Development.md) for architecture details, breakpoint/pause
data flow, Z80 disassembler design, PTY interface, and key source file map.

## License

[GNU General Public License v2.0](LICENSE)

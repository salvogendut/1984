# 1984 CPC Emulator — Development Notes

Architecture and implementation details for features beyond basic emulation.
For usage see [README.md](README.md).

---

## Memory Monitor / Debugger (F8)

Pressing **F8** opens (or closes) a separate SDL window: an 80×25
green-phosphor terminal with a `>_` prompt.  All commands are also
accessible via the PTY serial interface (`--monitor-pty`).

### Window layout

| Row(s) | Content |
|--------|---------|
| 0 – 22 | Scrolling output (23 lines) |
| 23     | Live register status bar |
| 24     | Command-input prompt (`>_`) |

Window size: **960 × 720** (4:3).  Achieved with a non-uniform SDL
render scale: `FONT_SCALE_X = 1.5`, `FONT_SCALE_Y = 3.6`, giving
12 × 28.8 px per character.

### Register status bar (row 23)

Rendered live on every frame from `CPC.cpu` — never stored in the
screen buffer.  Shows:

```
PC:XXXX SP:XXXX A:XX F:SZ-H-PNC BC:XXXX DE:XXXX HL:XXXX IX:XXXX IY:XXXX [PAUSED]
```

Background colour: dark green when running, dark red when paused.

### Commands

| Command | Description |
|---------|-------------|
| `D <addr> [<end>]` | Disassemble Z80.  Without end address, prints 10 lines.  Pageable with ENTER/SPACE. |
| `M <addr> [<end>]` | Hex + ASCII dump.  Without end address, fills the screen.  ASCII column is shown in reverse video. |
| `B [<addr>]` | Set a breakpoint at `addr`, or list all active breakpoints. |
| `BC <n>` | Clear breakpoint slot `n` (0 – 15). |
| `N` | Single-step one Z80 instruction (emulator must be paused). |
| `G` / `GO` | Resume execution (clear pause). |
| `GA` | Dump Gate Array: screen mode, border ink, all 16 inks. |
| `CRTC` | Dump all 18 CRTC registers plus live counters (MA, VLC, HCC, VCC). |
| `X` / `Q` | Close the monitor window. |

---

## Z80 Disassembler (`src/z80dis.c`)

Standalone C file, no external dependencies.  API:

```c
int z80dis(const u8 *mem, u16 pc, char *out, size_t outsz);
// Returns bytes consumed; out receives e.g. "LD HL,0B900h"
```

Decodes using the standard Z80 bit-field decomposition
(`x = op>>6`, `y = (op>>3)&7`, `z = op&7`) plus all prefix paths
(CB, DD, ED, FD, DDCB, FDCB).  The DDCB/FDCB path reads the
displacement byte *before* the final opcode byte.

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

When `stop_early` is true, `cycle_debt` is zeroed to prevent the
negative debt from skewing the next frame after resume.  Audio is
skipped to avoid a burst of silence samples when the frame is partial.

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

Starting the emulator with `--monitor-pty` opens a POSIX PTY master
(`posix_openpt`), configures it at 9600 baud raw, and prints the
slave device path to stderr:

```
1984: monitor PTY: /dev/pts/5  (minicom -b 9600 -D /dev/pts/5)
```

The PTY exposes the full command set.  Output lines that contain
reverse-video ASCII (hex dump) are wrapped with ANSI
`\033[7m` … `\033[0m` escape sequences.

`monitor_pty_tick()` is called once per frame from `main.c`.  It
reads up to 64 bytes per call (non-blocking), handles line
editing (backspace), echoes characters, and executes lines on `\r`
or `\n`.

---

## Key source files

| File | Role |
|------|------|
| `src/monitor.h` / `.c` | Monitor window, commands, PTY, status bar |
| `src/z80dis.h` / `.c` | Z80 disassembler |
| `src/cpc.h` / `.c` | Emulator core; breakpoint/pause logic lives in `cpc_frame()` |
| `src/joy.h` / `.c` | Joystick/gamepad input; SDL gamepad + raw joystick fallback |
| `src/main.c` | Event loop; F8 shortcut; post-frame pause detection |

---

## Joystick / input (`src/joy.c`)

SDL3 controllers are handled in two tiers: **gamepad** (devices in the SDL gamepad database, opened with `SDL_OpenGamepad`) and **raw joystick** fallback (all others, opened with `SDL_OpenJoystick`). Both support hot-plug. The raw tier maps axis 0 → Left/Right, axis 1 → Up/Down, button 0 → Fire 1, button 1 → Fire 2; hat switch is also handled. All inputs land in CPC keyboard matrix row 9.

`SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` is set to `"1"` before `SDL_Init` so gamepad events arrive even when the emulator window is not focused.

**PSG keyboard scan.** The CPC reads row 9 (joystick) through PSG I/O port A (register 14) via the PPI. When PPI port C bits 7–6 = `01` (PSG read mode), `cpc.c` reads `psg->kbd_data` directly rather than routing through `psg_read()`. This avoids a bug where software that does not explicitly pre-select PSG register 14 before the read (e.g. SymbOS) would receive `0x00` (all keys pressed) instead of the real matrix value.

Use `--trace-input` to log all keyboard and joystick events to stderr.

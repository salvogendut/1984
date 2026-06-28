# Auto-pilot input (`--pilot`)

A host-driven virtual input device. 1984 opens a pseudo-terminal (`/dev/pts/N`);
anything that can write lines to it — a shell script, a test harness, or an AI
given full control of the guest desktop during debugging — steers the **mouse
pointer** or the **CPC joystick** using polar-coordinate commands.

It is the live cousin of [`--joy-script`](../src/joy.h): where `--joy-script`
replays a fixed `DIRS:FRAMES` timeline, the pilot reacts to whatever the host
sends and holds the last velocity until told otherwise.

## Invocation

```bash
./1984 --pilot                       # open a PTY, target = mouse, no alias
./1984 --pilot=/tmp/1984-pilot.pty   # also create a stable symlink alias
./1984 --pilot=mouse                 # explicit initial target (default)
./1984 --pilot=joystick              # start driving CPC joystick 1
```

On startup 1984 prints the slave path and posts a toast:

```
1984: pilot PTY: /dev/pts/7 (target=mouse)
```

If the argument is `mouse`, `joystick`/`joy`, it selects the initial target;
any other non-empty argument is treated as a symlink path for a stable alias
(so you don't have to chase the `/dev/pts` number between runs).

Linux/POSIX only. On Windows the flag prints a notice and is ignored.

## Protocol

One command per line. Tokens are split on spaces or commas, command words are
case-insensitive, and `#` starts a comment. Unknown commands print a warning to
stderr and are ignored.

| Command | Meaning |
|---------|---------|
| `move R THETA` / `<R> <THETA>` / `v R THETA` | Set the velocity vector, **held until changed**. `R` = magnitude, `THETA` = angle in degrees. A bare line starting with a number is an implicit `move`. |
| `stop` (`s`, `halt`, `x`) | Set magnitude to 0 (keeps the last angle). |
| `press N` (`p N`) | Press button `N` (`1`=left/Fire1, `2`=right/Fire2, `3`=middle). |
| `release N` (`u N`) | Release button `N`. |
| `click N` (`c N`) | Press button `N` and auto-release after a few frames. |
| `scroll DZ` | Mouse wheel by `DZ` (mouse target only). |
| `target mouse` / `target joy` (`t m` / `t j`) | Switch target at runtime. |
| `reset` | Stop and release every button/direction. |

### Angle convention

`THETA` is in degrees, **0 = right, 90 = up, counter-clockwise** — the usual
mathematical orientation (the on-screen +y-is-down flip is handled internally).

### Magnitude semantics

- **Mouse target:** `R` is pixels per emulated frame (50 Hz), so `R=10` ≈
  500 px/s. Sub-pixel speeds accumulate across frames, so `0.5 0` still drifts
  right one pixel every other frame. The motion is fed to whichever mouse the
  guest has enabled (SymbiFace II and/or Albireo/CH376 HID), exactly like a real
  host mouse — if no mouse is enabled, motion goes nowhere.
- **Joystick target:** `THETA` snaps to the nearest of the 8 directions and any
  `R` above the deadzone engages it; magnitude is otherwise ignored (the CPC
  stick is digital). Buttons 1 and 2 map to Fire1 and Fire2.

## Examples

```bash
PTY=/tmp/1984-pilot.pty
echo "8 0"       > $PTY   # glide right at 8 px/frame
echo "8 90"      > $PTY   # turn upward
echo "click 1"   > $PTY   # left-click where we are
echo "stop"      > $PTY   # hold position
echo "target joy"> $PTY   # switch to the joystick
echo "5 270"     > $PTY   # press Down (270° = south)
echo "press 1"   > $PTY   # hold Fire1
echo "reset"     > $PTY   # neutral, release everything
```

## Implementation

- `src/pilot.c` / `src/pilot.h` — PTY setup mirrors `usifac.c`/`kbd_pty.c`;
  `pilot_tick()` runs once per frame from the main loop, next to
  `joyscript_tick()`.
- Mouse output calls `mouse_move`/`mouse_button`/`mouse_scroll` (SymbiFace II)
  and `ch376_mouse_move`/`ch376_mouse_button` (Albireo), gated on
  `cpc.symbiface_mouse` / `cpc.albireo_mouse`.
- Joystick output drives keyboard matrix **row 9** bits 0–5 via
  `kbd_key_down`/`kbd_key_up`, the same path as `joy.c`.

# Auto-pilot input (`--pilot`)

A host-driven virtual input device. 1984 opens a pseudo-terminal (`/dev/pts/N`);
anything that can write lines to it — a shell script, a test harness, or an AI
given full control of the guest desktop during debugging — drives the emulator
through a small **command/reply diagnostics protocol**.

For AI and harness use, `--pilot-replies-stderr` mirrors the structured reply
lines to stderr. That lets the host keep the PTY traffic write-only, which is
more reliable than a long-lived bidirectional slave session.

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
stderr and also return an `err ...` line on the PTY. Successful commands return
`ok ...`, while queries return `state ...`, `frame ...`, `hash ...`, or
`changed ...`. The PTY is therefore both the control plane and the cheapest
diagnostics surface.

When `--pilot-replies-stderr` is enabled, the same replies are mirrored as:

```text
1984: pilot reply: ok ...
1984: pilot reply: state ...
```

| Command | Meaning |
|---------|---------|
| `move R THETA` / `<R> <THETA>` / `v R THETA` | Set the velocity vector, **held until changed**. `R` = magnitude, `THETA` = angle in degrees. A bare line starting with a number is an implicit `move`. |
| `hold F R THETA` | Set the velocity vector for `F` frames, then auto-stop. |
| `stop` (`s`, `halt`, `x`) | Set magnitude to 0 (keeps the last angle). |
| `press N` (`p N`) | Press button `N` (`1`=left/Fire1, `2`=right/Fire2, `3`=middle). |
| `release N` (`u N`) | Release button `N`. |
| `click N` (`c N`) | Press button `N` and auto-release after a few frames. |
| `hold-click F N` | Press button `N` for `F` frames, then auto-release. |
| `scroll DZ` | Mouse wheel by `DZ` (mouse target only). |
| `key-down NAME` / `key-up NAME` | Press or release a CPC key by SDL scancode name. Examples: `A`, `Return`, `Left Shift`. |
| `key-tap NAME F` | Press the key for `F` frames, then release it. |
| `paste TEXT` | Queue `TEXT` through the existing CPC paste path. |
| `target mouse` / `target joy` (`t m` / `t j`) | Switch target at runtime. |
| `reset` | Stop and release every button/direction. |
| `state` | Emit a machine-readable state line on the PTY. |
| `frame` | Emit the current completed frame counter. |
| `hash` | Emit the current framebuffer hash. |
| `changed` | Emit the latest changed rectangle and quiet-frame count. |
| `wait frames N [T]` | Complete after `N` more frames, or timeout after `T` frames. |
| `wait hash-eq HEX [T]` / `wait hash-ne HEX [T]` | Wait on the framebuffer hash. |
| `wait change [T]` | Wait for any visible framebuffer change. |
| `wait quiet N [T]` | Wait for `N` consecutive quiet frames. |
| `crop PATH X Y W H [SCALE]` | Save a cropped screenshot region to `PATH`. |
| `snapshot-save PATH` / `snapshot-load PATH` | Save or restore an SNA snapshot. |

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
echo "hold 25 8 0" > $PTY # glide right for 25 frames, then stop
echo "8 90"      > $PTY   # turn upward
echo "click 1"   > $PTY   # left-click where we are
echo "hold-click 12 1" > $PTY   # hold button 1 for 12 frames
echo "stop"      > $PTY   # hold position
echo "target joy"> $PTY   # switch to the joystick
echo "5 270"     > $PTY   # press Down (270° = south)
echo "press 1"   > $PTY   # hold Fire1
echo "state"     > $PTY   # dump the current target/vector/button state
echo "hash"      > $PTY   # current visible framebuffer hash
echo "wait change 200" > $PTY     # block until something changes, max 200 frames
echo "crop /tmp/top.ppm 0 0 256 96 2" > $PTY   # 2x scaled crop
echo "snapshot-save /tmp/point.sna" > $PTY
echo "snapshot-load /tmp/point.sna" > $PTY
echo "reset"     > $PTY   # neutral, release everything
```

## Smoke Driver

`debug/ai_diag.py` is a minimal tracked host harness for this protocol. It:

- launches `./1984` headlessly,
- discovers the pilot PTY from stderr,
- sends pilot commands through one-shot write-only PTY opens,
- reads replies from `--pilot-replies-stderr`,
- prints replies verbatim,
- can run a simple smoke sequence (`--smoke`) or a line-based script (`--script`).

Example:

```bash
python3 debug/ai_diag.py --smoke -- --config=/dev/null --exit-after=200
```

## Implementation

- `src/pilot.c` / `src/pilot.h` — PTY setup mirrors `usifac.c`/`kbd_pty.c`.
  The protocol now runs in two passes: a pre-frame input/control pass and a
  post-frame diagnostics pass.
- Mouse output calls `mouse_move`/`mouse_button`/`mouse_scroll` (SymbiFace II)
  and `ch376_mouse_move`/`ch376_mouse_button` (Albireo), gated on
  `cpc.symbiface_mouse` / `cpc.albireo_mouse`.
- Joystick output drives keyboard matrix **row 9** bits 0–5 via
  `kbd_key_down`/`kbd_key_up`, the same path as `joy.c`.
- `src/display.c` provides cheap framebuffer observability: hash, changed-rect
  detection, and crop capture.

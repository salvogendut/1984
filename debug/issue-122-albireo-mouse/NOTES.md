# Issue #122 — Albireo USB HID mouse, picking up tomorrow

## Current state of the code (June 2026)

A previous pass landed an end-to-end USB-mouse implementation in commit
**`8034dab`** (May 2026, *"feat: Albireo USB HID mouse + UNIDOS file-open
polish"*). It is currently shipped on `main`. Nothing has reverted or
disabled it. The commit claims *"verified end-to-end against UNIDOS and
SymbOS"*, but user recollection (June 2026) is that the work was abandoned
mid-flight — so we need a fresh test to see whether it actually works or
quietly regressed.

### What is wired up today

| Layer                       | Where                                | Status |
|-----------------------------|--------------------------------------|--------|
| Struct fields `mouse_dx/dy/buttons` | `src/ch376.h:111-113`        | ✓      |
| Reset clears mouse state    | `src/ch376.c:360-361`                | ✓      |
| HID API `ch376_mouse_move/button`   | `src/ch376.c:887-898`        | ✓      |
| SDL → CH376 event wiring    | `src/main.c:631` (motion), `:644` (button) | ✓ |
| `SET_ADDRESS` (0x45) → SUCCESS | `src/ch376.c:664-667`             | ✓      |
| `SET_CONFIG`  (0x49) → SUCCESS | `src/ch376.c:664-667`             | ✓      |
| `ISSUE_TKN_X` (0x4E) on endpoint 1 → 3-byte HID report | `src/ch376.c:786-806` | ✓ |
| `SET_USB_ADDR` (0x13)       | `src/ch376.c:458`                    | ✓      |
| `SET_FREQ`    (0x0B)        | command table                        | ✓      |

### What the code expects vs. what SymbOS might actually do

The `ISSUE_TKN_X` handler matches **only** when the endpoint param byte is
`0x19` — i.e. endpoint 1, IN direction (`(ep & 0x0F) == 0x09 && (ep & 0xF0)
== 0x10`). Anything else returns `USB_INT_DISCONNECT` and SymbOS gives up.
Most likely failure modes:

1. **SymbOS polls a different endpoint.** Real Albireo USB mice may enumerate
   to endpoint 2 or 3 depending on the device descriptor; SymbOS may
   hard-code one address.
2. **Missing GET_DESCRIPTOR phase.** We don't currently implement
   `CTRL_TRANSFER`-style descriptor reads (device descriptor, config
   descriptor, HID report descriptor). If SymbOS tries to enumerate the
   device first, it gets `USB_INT_DISK_ERR` (default-case fall-through) and
   bails before ever issuing `ISSUE_TKN_X`.
3. **`SET_USB_MODE` flow mismatch.** UNIDOS picks `0x06`; SymbOS may pick
   `0x07` (host with SOF). Both are accepted today but may need different
   subsequent state.
4. **Idle-poll convention.** When no mouse data is available, SymbOS may
   expect `USB_INT_USB_NAK` (`0x2A`) or similar, not the SUCCESS-with-zero
   report we may currently send (the code returns the 3-byte report
   unconditionally even when `dx==dy==0`).

## Plan for tomorrow

1. **Capture a real trace.** Run `debug/issue-122-albireo-mouse/trace.sh
   [path-to-albireo-image]` — boots a 6128 with Albireo enabled and
   `--trace-albireo` logging every CH376 command + interrupt code into
   `trace.log` alongside this NOTES.md.
   - Click in the window to engage mouse capture.
   - Boot SymbOS from the Albireo volume.
   - Move the mouse a few times once on the desktop.
   - Click left/right.
   - Quit (F12 or close window).

2. **Read the log.** Look for, in order:
   - Did SymbOS get past `SET_USB_MODE`? (any disconnect after that point
     means descriptor reads probably failed)
   - Are there any `DISK_ERR` lines? Those are unhandled commands SymbOS
     issued — those are our gaps.
   - Do `ISSUE_TKN_X` lines show up at all? If yes, with what endpoint byte?
   - If `ISSUE_TKN_X` polls come in but DISCONNECT goes back, the endpoint
     mask is wrong — log the actual byte and widen the match.

3. **Decide direction.**
   - If SymbOS *never* reaches `ISSUE_TKN_X` → we need to implement minimal
     USB descriptor responses (likely via a new `CTRL_TRANSFER`-style
     command path).
   - If it reaches `ISSUE_TKN_X` but with a different endpoint → small
     constant fix.
   - If it reaches and returns success but the mouse still doesn't move on
     screen → likely a polarity/scaling issue in the HID report bytes (the
     code already clamps to int8; check whether SymbOS expects unsigned bias
     instead).

## References

- `--trace-albireo` flag — primary debug signal.
- `src/ch376.c` — current emulation; command dispatcher around line 458 and 664.
- UNIDOS source (Albireo node, for protocol reference):
  `~/Downloads/UNI/SOURCE/ALBIREO/src/LowLevel.a`
- Albireo hardware doc:
  https://pulkomandy.github.io/shinra.github.io/albireo.html
- ALBIREO.md — documents the *storage*-side SymbOS issue (text-vs-apps
  tradeoff via `albireo_disable_disk_read`); orthogonal to mouse but useful
  context.

## Out of scope

- SC16C650B UART (NVRAM). Separate concern.
- The SymbOS-on-Albireo storage rendering tradeoff (already documented in
  ALBIREO.md). Disabling Albireo storage via `albireo_disable_disk_read=true`
  in `1984.conf` may help isolate the mouse path from the storage bug
  during testing.

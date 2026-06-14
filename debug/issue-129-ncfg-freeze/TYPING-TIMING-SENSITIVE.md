# #129 — race is `|hdcpm`-arrival-frame sensitivity (2026-06-14)

User ran 10 interactive HDCPM boots with `ONE_K_TRACE_SDL_EV=1` and
recorded which boots reached A0:CPM> (5) vs which cold-reset back to
BASIC (5). Trace logs at `/tmp/boot-run01.log` through `run10.log`.

## The decisive finding

Per-run event types and counts are essentially **identical** between
PASS and FAIL runs. The difference is **when** the user's manual
typing of `|hdcpm` lands relative to the CPC's clock:

```
First 'H' KEY_DOWN frame per run:
  run01 (FAIL): frame=1102
  run02 (PASS): frame=1085
  run03 (FAIL): frame=1488
  run04 (PASS): frame=3323
  run05 (PASS): frame=1109
  run06 (FAIL): frame=1203
  run07 (PASS): frame=1106
  run08 (FAIL): frame=1339
  run09 (PASS): frame=1655
  run10 (FAIL): frame=1321
```

PASS and FAIL frames are interleaved (run07 passes at f1106, run01
fails at f1102 — only 4 frames apart!). So this isn't a coarse
"early-vs-late" boundary — it's **fine-grained, sub-frame-resolution
alignment sensitivity**. Some specific `|hdcpm`-arrival frames make
HDCPM boot cleanly; nearby ones make it cold-reset.

## What this means for the bug

The residual #129 race is **not** in CP/M+ kernel runtime stability
(we already showed pure-CLI runs are 5/5 bit-identical with the LDIR
fix). It's in **HDCPM's boot sequence being timing-sensitive to when
the `|hdcpm` RSX is invoked**. Specifically, the keystroke that
triggers HDCPM lands at a CPC clock state, and HDCPM's subsequent
disk-scan / CP/M+ kernel handoff hits race windows that depend on
that initial alignment.

Earlier confirmed (pure-CLI testing):
- With ONE_K_AUTOSTART_FRAMES=1500 + `--paste="|hdcpm\n"`, 5/5 runs
  produce **bit-identical** results. 1984's emulation is fully
  deterministic given fixed inputs.
- With `--ocr-monitor` added, runs differ (the OCR pipeline itself
  introduces non-determinism in the probe harness, separate problem).

So the user's "7/10 success interactively" wasn't due to 1984 being
non-deterministic. It was due to human finger-timing variability.

## Open question for "konCePCja-level precision"

Does konCePCja have the same `|hdcpm`-arrival-frame sensitivity? Two
possibilities:

1. **konCePCja crashes at the same timings 1984 does**. Then the
   timing-sensitivity is a real HDCPM property (it would also crash
   on real Cyboard hardware sometimes). 1984's behavior matches the
   reference and we're done — but the bug is in HDCPM itself.
2. **konCePCja boots HDCPM at ALL `|hdcpm`-arrival timings.** Then
   1984 has a genuine cycle-accounting bug that konCePCja doesn't.
   The cycle-table or IRQ-acceptance timing differs in a way that
   only manifests for some startup alignments.

Test plan: drive konCePCja headlessly with `|hdcpm` at sweeping
autostart frames (1085, 1102, 1106, 1109, 1203, 1321, 1339, 1488,
1655, 3323 — the actual frames from our 10 manual runs). For each,
classify PASS/FAIL. Compare with 1984's classification at the same
frames.

If konCePCja is 10/10 PASS at all those frames → 1984 has a real bug
worth more cycle work. If konCePCja is also ~5/10 → we've matched
the reference and the rest is HDCPM-side.

## Path to making this disappear for users (if konCePCja matches us)

- **Document the sensitivity**: HDCPM boot can occasionally crash
  if `|hdcpm` is typed at the "wrong" frame; just press F5 (reset)
  and retype. Same as real hardware.
- **Auto-boot via TAB**: HDCPM's ROM init checks the TAB key —
  if held at boot, it auto-runs `|hdcpm` instead of returning to
  BASIC. Frame-aligned start, no typing variability. 1984 could
  expose this via a `--autoboot-hdcpm` flag.

## Path to a real fix (if konCePCja outperforms us)

- More cycle-table corrections, guided by konCePCja's behavior at
  the specific failing alignments.
- Bus-tick infrastructure (the reverted 5-commit stack) might
  finally pay off if pointed at the right opcode.

## Reproducer with fixed frame (no human typing)

```bash
env -u WAYLAND_DISPLAY DISPLAY=:99 SDL_VIDEODRIVER=x11 \
    ONE_K_FAKE_RTC=1 ONE_K_FAKE_RTC_TIME=12:00:00 \
    ONE_K_AUTOSTART_FRAMES=$FRAME \
    ./1984 --memory=576 --config=debug/issue-129-ncfg-freeze/test-nonet.conf \
    --paste="|hdcpm
" --screenshot-at=4500:/tmp/test-$FRAME.ppm

# Classify by screenshot — A0:CPM> visible = PASS, BASIC banner = FAIL.
# Sweep FRAME across 1080..1700 in steps of ~5 to map the failure landscape.
```

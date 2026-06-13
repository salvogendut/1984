# Issue #129 — NCFG-freeze investigation harness

Autonomous probe for the residual race that occasionally crashes the CP/M+
kernel during `ncfg -r` / `ncfg -a:cpc` / `ping`. Drives 1984 end-to-end:

- Pastes `|hdcpm`, waits for CP/M+ to boot.
- Pastes `ncfg -r`, `ncfg -a:cpc`, `ping -c5 slashdot.org` at fixed frame
  intervals via the `--kbd-pty` keyboard PTY.
- Takes a screenshot at frame 11000 (~220 s wall) via `--screenshot-at`.
- OCRs the screenshot with `tesseract` (in the project's distrobox) to
  classify each run: `PASS_PING`, `POST_NCFG_NO_PING`, `REBOOTED`, etc.
- Captures `ONE_K_TRACE_LBA` + `ONE_K_TRACE_IM1` traces per run for
  diff-based comparison.

## Usage

```bash
debug/issue-129-ncfg-freeze/probe.py [N]
```

`N` defaults to 1. Artifacts land in `/tmp/ncfg-probe/`:

- `run<i>.ppm` / `.png` — final-state screenshot (the actual evidence).
- `run<i>.log` — emulator stderr (IDE LBA + IRQ-PC trace).
- `run<i>-report.txt` — OCR of the screenshot plus classification.

Typical observed distribution on the user's setup: ~60% `PASS_PING`,
~30% `REBOOTED`, ~10% `POST_NCFG_NO_PING`.

## How the failure looks

All `REBOOTED` runs are bit-identical down to the IDE LBA sequence
(1571 cmds, ending at a re-mount sweep `0x123-0x222`). The CPU enters
a long block instruction at PC `0x0B2C` inside NCFG / kernel code; ~28
consecutive IM1 IRQs fire at that exact PC during a single LDIR-style
loop, and at some iteration state gets corrupted and HDCPM re-mounts.
Same fingerprint as the original race partially closed in PR #128.

## Why this needs `--kbd-pty`

The CP/M+ kernel writes its console output directly to screen RAM
without going through the firmware `&BB5A` vector, so a generic text
hook only catches HDCPM-ROM banner output (everything up to "Hard
disk N mounted") and goes blind once CP/M+ takes over. We use the PTY
for typed input — which the kbd matrix sees identically regardless of
what's reading it — and rely on the final screenshot+OCR for state
verification. A proper screen-RAM scrape would let the probe react
mid-flight to the freeze instead of inferring it from the final
frame, but that's a separate task.

# #129 — follow-up findings (2026-06-14)

This file revises the earlier `MORNING-REPORT.md`. Several of its
conclusions turned out to be contaminated by **probe-harness bugs**,
not real emulator behaviour.

## Headline correction

The "30% reboot rate" and "Net4CPC OFF boots 0/10" data that drove the
earlier TAP-timing hypothesis were dominated by two harness bugs:

1. **The probe.py `Pty` wrapper used one O_RDWR fd shared between a drain
   thread and the send path.** Concurrent read+write on the same kernel
   file struct silently dropped the keystroke writes. With Net4CPC ON,
   the run was a little slower so the race resolved more often (PASS);
   with Net4CPC OFF, the race almost always lost (PROMPT_FAIL).
2. **`screen-text` OCR mistook the BASIC cursor block for `?`.** The
   probe's PROMPT_FAIL "screen shows `Ready / ?`" was interpreted as a
   syntax-error response when actually it was just BASIC sitting at
   Ready with cursor on — `|hdcpm` had never been received at all.

When the harness is fixed (single invocation, split-fd Pty), Net4CPC
OFF boots cleanly to the CP/M Plus banner via PTY input — confirmed by
true-pixel screenshot. No CPU race needed.

## What still stands from `MORNING-REPORT.md`

- **PR #128 boot fix is genuine.** Confirmed 10/10 via boot-only check
  with Net4CPC ON, and reproducibly via standalone twin-test runs.
- **iWSAdjust LDIR experiment was a regression** — that part of the
  notes stays.
- **Wall-clock RTC introduces minor IRQ-trace non-determinism**
  independent of TAP. With `ONE_K_FAKE_RTC=1`+`ONE_K_FAKE_RTC_TIME=…`,
  2/3 consecutive runs become bit-identical; the remaining 3rd run
  diverges from a still-unidentified source (task #57).

## What this session shipped

1. **`--config=PATH` CLI flag** so tests can pin their own config
   instead of mutating the user's interactive one.
   `debug/issue-129-ncfg-freeze/test-nonet.conf` is the pinned probe
   config (Net4CPC OFF, everything else as user's).
2. **Split read/write fds in probe.py `Pty`** — was the root cause of
   the apparent Net4CPC-OFF boot failure. Confirmed by reproducer in
   `/tmp/twin-pty-test.py` (works) vs single-O_RDWR version (drops bytes).
3. **`ONE_K_TRACE_KBDPTY`** — per-byte stderr trace at kbd_pty_tick's
   read site, with `fflush(stderr)` so SIGKILL doesn't eat it. This was
   the diagnostic that unmasked the harness bug; keep it on for any
   PTY-suspicious failure.

## Residual harness bug (task #58)

`boot_only_check.py N>1` still drops bytes on iterations 2+. Same
spawn flow works perfectly when reproduced in a standalone
`/tmp/twin-wait-for.py` script — including the `wait_for("Ready")`
polling loop — so the bug is in *something specific to
boot_only_check.py's interaction with the probe's iteration cleanup*
that I haven't isolated yet.

Workaround until that's debugged:

```bash
for i in 1 2 3 4 5; do
    pkill -9 -f "1984 --memory" || true; sleep 2
    rm -rf /tmp/boot-probe
    python3 debug/issue-129-ncfg-freeze/boot_only_check.py 1
    mv /tmp/boot-probe/run1.log /tmp/boot-probe-trial$i.log
done
```

Each shell-loop iteration starts a fresh Python process, so the
inter-iteration state leak (whatever it is) doesn't accumulate.

## Re-baseline the real #129 once batch mode works

With both the data-loss bug and the cursor-OCR ambiguity fixed, the
*actual* probabilistic-failure rate of NCFG/ping needs to be remeasured
from scratch. The "frame 60 TAP-divergence" finding in
`MORNING-REPORT.md` should be treated as inconclusive until then — the
divergence may genuinely come from host-kernel scheduling, or it may
just be measurement noise from the same family of harness bugs.

Specifically, the next clean batch should:

1. Use the split-fd `Pty` (fix shipped today).
2. Freeze the RTC (`ONE_K_FAKE_RTC`).
3. Run N=30 from a shell loop, one Python process per iteration.
4. Score PROMPT_FAIL **only** when a screenshot at frame 4000+ actually
   shows BASIC banner (i.e. the machine genuinely reset) — not just
   based on screen-text reader output, since the `?` glyph ambiguity
   confused the earlier classification.

# #129 — Overnight investigation report

Branch: `129-investigate`. Pushed but NOT merged.

## TL;DR for headline reading

**The nondeterminism source isn't a Z80 emulation race — it's host-kernel
TAP packet timing.** Two runs of the same workload (same probe, same
config, same image) diverge at IRQ index 357 (frame 60, 1.2 s into the
run), regardless of whether the run eventually PASSes or FAILs.
Verified by `compare_traces.py` on three different log pairs:
- PASS vs FAIL: diverges at IRQ 357.
- PASS vs PASS: ALSO diverges at IRQ 357.
- FAIL vs FAIL: same — frame 60.

This means the residual #129 race is a **code-path race inside HDCPM /
CP/M+ kernel that's *triggered* by how/when the host kernel delivers
TAP packets**. The kernel either makes it through the race window or
doesn't, depending on which packets arrived by the time it ran a given
NCFG / DHCP / ARP loop. Z80 cycle-count fixes don't address this.

The "fix" shape would have to make TAP responses *deterministic* in
emulator time — buffer all TAP packets and deliver them at fixed
instruction intervals, or mock the TAP with a deterministic responder
during tests. Both are significant architectural changes, not single-
evening overnight work.

## Summary

- **Bigger picture confirmed**: Our Z80 timing model sits in a local minimum
  that doesn't tolerate single-axis alignment toward caprice32/konCePCja.
  Tried adding LDIR/LDDR/LDI/LDD + LD A,I/R + LD I/R,A iWSAdjust bumps
  (a real reference-vs-1984 gap); regressed HDCPM boot from 40/40 to
  PROMPT_FAIL by run 2 of the validation batch. Reverted. **More
  importantly: the failure surfaced before the iWSAdjust shift could
  matter — divergence happens at frame 60 from TAP timing.**

- **Baseline measured**: N=20 headless probe → 12 PASS / 4 FREEZE_NCFG_A
  / 1 FREEZE_PING / 2 HDCPM_FAIL / 1 PROMPT_FAIL = 60% pass / 40% fail.
  Headless is harder than interactive (the user normally sees ~30% fail);
  Xvfb's framerate/timing differs subtly from real display.
- **Investigation harness substantially improved**:
  - probe.py runs headless under Xvfb (`--display :99`) so windows don't
    pop on the user's session.
  - Uses prompt-count (`CPM>` substring frequency) for OCR-tolerant
    command-completion detection.
  - Aggressive cleanup between runs — found a SIGTERM-only leak that
    held cpc-tap0 and caused ALL subsequent runs to FREEZE_NCFG_A.
  - `compare_traces.py` finds first IDE+IRQ-PC divergence between PASS
    and FAIL traces.
  - `boot_only_check.py` regression guard — confirms PR #128's 40/40
    realcyboard boot rate hasn't moved.
- **Baseline measurement**: N=20 batch run on the cleaned-up probe;
  see `investigation-notes.md` for the final tally.

## What was tried

See `investigation-notes.md` for chronological detail. Brief:

1. **Experiment 1** (reverted): caprice32's full iWSAdjust list for ED
   block-move and I/R-register ops. Reasonable per the reference, but
   adding it alone shoves the local-minimum timing into a worse spot
   and breaks the HDCPM boot path PR #128 closed. Same lesson as
   earlier iWSAdjust-only / IM1-only / VSYNC-only attempts.

## What's NOT been tried (recommended for next session)

Given the host-TAP-timing finding, the *useful* angles are different
from what we'd assumed earlier:

1. **Deterministic TAP responder** — biggest lever. Replace the live
   `tap_read()` path with one that schedules outbound packets at fixed
   instruction counts (or T-state counts) inside the emulator. Two PASS
   runs would then produce bit-identical traces, and we could actually
   diff PASS vs FAIL traces to find a Z80-level race (if one even
   exists). Without this, any CPU-timing change is investigated against
   a noise floor that swamps the signal. ~1-2 days work.

2. **Audit `n4c_stack_poll()`** for non-idempotent behaviour. If poll
   has any state mutation that depends on packet arrival order, that's
   one knob to flatten. Reading `src/n4c_stack.c` line 756 — there is
   side-effecting state (ARP cache, TCP CBs); these get updated based
   on what the host kernel sends and *when*. Documenting the per-poll
   side-effects would help anyone tackling the deterministic-responder
   work.

3. **Confirm by disabling Net4CPC**. A boot+ping batch with TAP off
   (use the legacy host-socket fallback in net4cpc.c) should be
   *deterministic* — same code path every time. If THAT case still
   shows non-zero fail rate, there's a residual Z80 race independent
   of TAP. If it's 0% fail, all of #129 is TAP-driven.

4. **Atomic port of full bus-contention model** from konCePCja — still
   the right thing for a CPU-level fix if one is needed, but lower
   priority now that we have evidence TAP is the dominant source.

## Concrete next steps

In priority order:

1. ✅ **boot-only regression check: 10/10 PASS.** PR #128's boot rate is
   intact under headless. The HDCPM_FAIL / PROMPT_FAIL we see in the
   probe.py baseline are probe-time noise, not actual 1984 regressions.
2. ⚠️ **net4cpc_tap=false test inconclusive.** Disabling TAP just kills
   the built-in DHCP server too, so NCFG -a:cpc legitimately hangs (no
   responder). N=8 with TAP off: 8/8 FREEZE_NCFG_A — but that's because
   there's nothing to respond, not because the race isn't TAP-driven.
   A clean confirmation test would need a deterministic mock TAP
   responder (which is exactly the "fix" shape).
3. **Build the deterministic-TAP-responder** if pursuing the bug. ~1-2
   days. Outline:
   - Add `n4c_stack_set_test_clock(cycle_count_callback)` so the stack
     knows the current emulator time.
   - Add `n4c_stack_schedule_packet(deliver_at_cycles, packet)` so test
     code can queue DHCP-OFFER / ARP-REPLY at deterministic moments.
   - In test mode, `tap_read()` returns those scheduled packets instead
     of reading the real TAP fd.
   - probe.py uses test mode; production runs use live TAP unchanged.
4. **Atomic port of the full bus-contention model** from konCePCja as
   a separate, larger project. Still wise to do at some point; only
   addresses the residual CPU-level race that *might* still be there
   once TAP is deterministic.

## How to use what shipped

```bash
# Headless probe with proper cleanup
./debug/issue-129-ncfg-freeze/probe.py 20

# Verify PR #128 boot hasn't regressed before any timing change
./debug/issue-129-ncfg-freeze/boot_only_check.py 10

# Diff a PASS vs FAIL trace to find the divergence point
./debug/issue-129-ncfg-freeze/compare_traces.py \
   /tmp/ncfg-probe/runPASS.log /tmp/ncfg-probe/runFAIL.log
```

All three rely on the kbd-PTY + OCR-monitor + monitor-PTY infrastructure
shipped in PRs #133 / #134.

## Files in this branch

- `src/screen_text.c` — hamming-distance fuzzy match for CP/M+
  kernel-font glyphs (the only code change that didn't get reverted).
- `debug/issue-129-ncfg-freeze/probe.py` — improved (Xvfb, prompt count,
  cleanup).
- `debug/issue-129-ncfg-freeze/boot_only_check.py` — boot regression guard.
- `debug/issue-129-ncfg-freeze/compare_traces.py` — PASS/FAIL divergence
  finder.
- `debug/issue-129-ncfg-freeze/investigation-notes.md` — detailed
  pass-by-pass record + table of every iWSAdjust-bump site in
  caprice32 / konCePCja / 1984.
- `debug/issue-129-ncfg-freeze/MORNING-REPORT.md` — this file.

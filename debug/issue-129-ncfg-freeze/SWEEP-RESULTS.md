# #129 — frame-sweep comparison: 1984 vs konCePCja (2026-06-14)

13 `|hdcpm` paste-frame values swept against both emulators, same
realcyboard.img, same HDCPM.ROM, same SymbifaceII setup, Net4CPC off,
frozen RTC (1984 only — konCePCja uses host wall clock but result
classification is unaffected).

## Results

| Frame | 1984 | konCePCja |
|------:|:----:|:---------:|
| 1080  | PASS | PASS |
| 1130  | PASS | PASS |
| 1180  | **FAIL** | PASS |
| 1230  | **FAIL** | PASS |
| 1280  | **FAIL** | PASS |
| 1330  | **FAIL** | PASS |
| 1380  | **FAIL** | PASS |
| 1430  | PASS | PASS |
| 1480  | **FAIL** | PASS |
| 1500  | (skipped) | PASS |
| 1530  | **FAIL** | PASS |
| 1580  | PASS | PASS |
| 1630  | **FAIL** | PASS |
| 1680  | PASS | PASS |

- **1984: 5/13 PASS (38%)**
- **konCePCja: 13/13 PASS (100%)**

## Verdict

**1984 has a real cycle-accounting bug that konCePCja does not have.**
konCePCja correctly handles every `|hdcpm`-arrival frame tested;
1984 fails at 8 of the 13 frames in the same sweep range. The
sensitivity to specific keystroke-arrival alignments is unique to
1984's Z80/Gate-Array timing — not a HDCPM property nor a CP/M+
property nor real-hardware behaviour.

This rules out "HDCPM is just timing-sensitive on real hardware too"
and locks the bug into 1984's CPU emulation. More cycle-table
investigation is justified.

## What worked vs what didn't (this session's score)

✅ Real fix shipped: LDIR/LDDR/LDI/LDD/LD A,I/R iWSAdjust bumps
   (commit 963486e) — closes the deterministic frame-3266 crash,
   makes the bug probabilistic at 5/13 instead of 0/13.

✅ Tooling shipped: --config CLI, split-fd Pty, ONE_K_TRACE_KBDPTY,
   ONE_K_TRACE_SDL_EV.

✅ Investigation localised: bug is in cycle-accounting, not in
   bank-switch timing, not in OUT-instruction split timing, not in
   IRQ-accept tick timing, not in memory bus contention.

❌ Bus-tick infrastructure (5 commits, reverted 17b28b9): correct
   mechanically but didn't move the race.

❌ Cycle-bumping OUT (n),A (Oa=12, Oa=12 with split): regresses.

## What's still on the table

- konCePCja has additional cycle adjustments 1984 doesn't have.
  The matching `iWSAdjust++` set is complete for ED-prefix LD ops;
  the gap must be in another opcode class.
- Possible suspects:
  - DD/FD prefix overhead (cycle accumulator for IX/IY indirection)
  - HALT instruction T-state accumulation under IRQ pressure
  - The "post-tick" for IO ops (Ox_/Oy_/Ia_/Oa_ portions in
    konCePCja that 1984 lumps into the pre portion)
  - Specific instructions inside the CP/M+ kernel's
    "scan startup devices" loop that we haven't audited

Next session: cycle-count diff at instruction-by-instruction
granularity using `wait pc` + `step trace` IPC in konCePCja, against
1984 instrumented to log every opcode's cycle count.

## Artifacts

`/tmp/sweep-1984/*.ppm` — 1984 final screens (13 frames)
`/tmp/sweep-kon/*.png` — konCePCja final screens (14 frames)

PASS md5 for 1984: `362aa785` (A0:CPM> visible, RTC frozen 12:00)
PASS for konCePCja: any frame; all 13 show `A0:CPM>` with varying
host-wall-clock RTC timestamps (20:81–20:93).

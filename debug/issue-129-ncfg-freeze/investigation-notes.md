# #129 NCFG/ping race — autonomous investigation notes

Working branch: `129-investigate`.

## Pass 1 — Baseline

Probe: `debug/issue-129-ncfg-freeze/probe.py 30` against `main` after PR #134 +
the hamming-distance screen-text tweak. Runs under Xvfb on `:99` headless.

Baseline result: **pending — N=30 batch in progress.**

## Pass 2 — Z80 audit (started while baseline runs)

### Finding: missing iWSAdjust bumps on ED block-move opcodes

Comparing `src/z80.c` against `/var/home/salvogendut/Dev/caprice32/src/z80.cpp`
and `/var/home/salvogendut/Dev/konCePCja/src/z80.cpp`:

| Opcode  | caprice32 (z80.cpp:2408-2411) | konCePCja (z80.cpp:2574-2575) | 1984 (z80.c:662-666) |
|---------|-------------------------------|-------------------------------|----------------------|
| LDI     | `iWSAdjust++`                 | `iWSAdjust++`                 | **NOT bumped**       |
| LDD     | `iWSAdjust++`                 | `iWSAdjust++`                 | **NOT bumped**       |
| LDIR    | `iWSAdjust++` (every iter)    | `iWSAdjust++` (every iter)    | **NOT bumped**       |
| LDDR    | `iWSAdjust++` (every iter)    | `iWSAdjust++` (every iter)    | **NOT bumped**       |
| LD A,I  | `iWSAdjust++` (line 2412)     | matched                       | **NOT bumped**       |
| LD A,R  | `iWSAdjust++` (line 2413)     | matched                       | **NOT bumped**       |
| LD I,A  | `iWSAdjust++` (line 2422)     | matched                       | **NOT bumped**       |
| LD R,A  | `iWSAdjust++` (line 2423)     | matched                       | **NOT bumped**       |
| CPI     | `iWSAdjust++`                 | matched                       | NOT bumped           |
| CPD     | `iWSAdjust++`                 | matched                       | NOT bumped           |
| CPIR    | `iWSAdjust++` (every iter)    | matched                       | bumped on taken ✓    |
| CPDR    | `iWSAdjust++` (every iter)    | matched                       | bumped on taken ✓    |

**Background**: I excluded LDIR/LDDR earlier in PR #128 based on the macro
definition (`#define LDIR LDI; if(_BC) { ... }`) which doesn't increment
iWSAdjust inside the macro. The dispatcher at z80.cpp:2411 does the bump
*after* calling the macro. Same for CPIR/CPDR — the macro bumps internally
but the dispatcher also bumps. For CPIR/CPDR I caught it; for LDIR/LDDR I
missed it.

PC=0x0B2C in the fail trace is precisely an LDIR/LDDR-like long block
instruction (28 consecutive IM1 accepts at the same PC). The missing
iWSAdjust bump means our IRQ accept cost is 20 T-states instead of 16
during those iterations — a 4-cycle drift per IRQ over hundreds of IRQs,
exactly the kind of accumulated phase error that lands the IRQ in the
wrong place in the kernel's bank-save/restore code.

### Planned fix (Pass 4)

Add the missing ED-prefix opcodes to the iWSAdjust bump table in
`z80_step()` (around src/z80.c:662):

- `0xA0` (LDI), `0xA8` (LDD): bump always
- `0xB0` (LDIR), `0xB8` (LDDR): bump always (not just on `taken` — even
  the final iteration bumps per caprice32/konCePCja)
- `0xA1` (CPI), `0xA9` (CPD): bump always
- `0x57` (LD A,I), `0x5F` (LD A,R), `0x47` (LD I,A), `0x4F` (LD R,A):
  bump always

The CPIR/CPDR cases already handled — `bump = taken` is technically
narrower than the reference (which bumps unconditionally for CPI/CPD,
CPIR/CPDR), but a non-taken CPIR is just a CPI semantically so the
"always-bump" path subsumes it. Will widen to `bump = true` for
consistency.

## Pass 3 / 4 — Targeted experiments

### Experiment 1: LDIR/LDDR/LDI/LDD + LD A,I/R + LD I/R,A iWSAdjust bumps

Apply caprice32 + konCePCja's missing iWSAdjust bumps on the ED block-move
ops + I/R register transfers (see table above).

Patch: extended the ED-prefix switch in `z80_step` at src/z80.c around
line 662. Added cases 0xA0, 0xA8, 0xB0, 0xB8 (LDI/LDD/LDIR/LDDR), 0xA1,
0xA9 (CPI/CPD), and 0x47/0x4F/0x57/0x5F (LD I/R,A and LD A,I/R) with
`bump = true` unconditionally. Widened CPIR/CPDR from `bump = taken` to
`bump = true` so they always bump (matches caprice32 dispatcher; the
caveat-on-not-taken in the macro was a narrowed view).

Build: clean.

Result: **REVERTED in commit 9a9d056 — regressed HDCPM boot.**

In the first 3 runs of the N=10 batch:
- run 1: FREEZE_NCFG_A (plausible — DHCP slow).
- run 2: **PROMPT_FAIL — A0:CPM> never appeared.** HDCPM banner showed
  but CP/M+ never reached the prompt. **Boot-time regression.**

PROMPT_FAIL is exactly the class of regression PR #128 closed for the
realcyboard case. Can't ship.

Why it hurts: the bump table adds LDIR/LDDR — used *heavily* during
HDCPM's CPMDSKxx.IMG mount + CP/M+ kernel relocation. Each LDIR
iteration shaves 4 cycles off the *next* IRQ accept; over the
kernel's hundred-thousand-iteration LDIR moves, that's an enormous
cumulative phase shift, landing the 50 Hz IRQ in a worse place than
un-bumped.

The reference (caprice32/konCePCja) IS technically correct, but inside
their *coupled* timing model (IM1=20 baseline + full iWSAdjust list +
... + ... full bus contention). Our model is partial: IM1=20 + partial
iWSAdjust + cc_op tables, no bus contention. Patching the reference's
LDIR bump alone breaks the equilibrium PR #128 established. Same
lesson learned three times now (iWSAdjust-only, IM1-only, LDIR-bump):
**any single-axis CPU-timing change against our partial model
regresses the working path.**

### Implications for tomorrow's report

This pass has effectively *confirmed* what PR #128 hinted at: our Z80
timing model sits in a local minimum that doesn't tolerate piecemeal
alignment toward the reference. The right fix is one of:

a) Port the full bus-contention coupling from konCePCja as a single
   atomic change (large project — days, not hours).
b) Find a *non-timing* contributing factor — host TAP packet timing,
   memory aliasing, IDE-register side effect — that's making the race
   fire 30% rather than 0%, since the underlying timing race is
   fundamental to the local-minimum we've landed in.
c) Accept the ~30% rate as the residual and document it.

### Next angles to investigate (overnight, lower priority)

These don't risk regressing the working path because they're not
single-axis CPU timing tweaks:

1. **Net4CPC TAP poll cadence**: maybe the residual race is host-TAP
   side, not Z80 side. We poll once per frame; konCePCja polls more
   aggressively. Different DHCP/ARP arrival times → different IRQ
   alignment.
2. **Snapshot-resume harness**: cut probe per-run cost from 3 min to
   30 s by saving a snapshot at A0:CPM> prompt and resuming for each
   run. Makes any future N=30 batch feasible in ~15 min.
3. **Audit non-timing emulation differences**: IDE status bits, IRQ ack
   timing on the GA side, RTC tick correctness. Anything that could
   change *deterministically* between PASS and FAIL runs.

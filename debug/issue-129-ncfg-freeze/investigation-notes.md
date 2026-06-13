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

Result: **pending — N=10 probe in progress.**

Acceptance criterion:
- Pass rate ≥ baseline (no regression).
- `boot_only_check.py` pass rate ≥9/10 (PR #128 regression guard).
- If pass rate increases above baseline, run N=30 to confirm.

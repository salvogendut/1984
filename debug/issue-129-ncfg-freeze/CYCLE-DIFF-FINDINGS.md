# #129 — cycle-accounting diff between 1984 and konCePCja

After commit `963486e` ("z80: bump iWSAdjust for LDI/LDD/LDIR/LDDR +
LD A,I / LD A,R"), HDCPM/CP/M+ boot reaches the `A0:CPM>` prompt in
1984 (was impossible — frame 3266 deterministic crash). Stability is
better but still imperfect.

This document records what else differs between the two emulators'
cycle bookkeeping and what to try next.

## What's the same

- `cc_op[]`, `cc_cb[]`, `cc_ed[]`, `cc_xy[]` tables: **identical** byte-
  for-byte (verified with `diff`).
- Cycle-cost constants `Oa=8, Ia=12, Ox=8, Oy=12, Ix=12, Iy=16`:
  identical.
- IM1 acceptance = 20 cycles (16 with iWSAdjust); IM2 = 28 (24 with
  iWSAdjust): identical.
- GA `interrupt_counter &= 0x1F` on IRQ ack: done correctly in 1984
  (`src/cpc.c:1138`).

## What 1984 still differs from konCePCja in

### 1. `iWSAdjust++` for `LD I,A` / `LD R,A`

konCePCja bumps for these (z80.cpp:2586-2587):

```cpp
case ld_i_a:      _I = _A; iWSAdjust++; break;
case ld_r_a:      _R = _A; _Rb7 = _A & 0x80; iWSAdjust++; break;
```

When I added these in 1984 (matching konCePCja's behaviour), the HDCPM
boot **regressed** — the system ended up at BASIC Ready earlier than
without the bumps. Why this exact pair causes a regression while the
six already-added bumps don't is the open question.

Working theory: CP/M+ executes `LD I,A` exactly once during IM2
initialisation, and that single bump shifts the IRQ-acceptance window
relative to the kernel's "Scanning startup devices" loop so that a
different race triggers. Investigating would need a precise trace of
the I register's value and the IM2 vector page over time.

### 2. `z80_wait_states` splitting around IO operations

konCePCja's IO ops split their cycle accounting: cycles run BEFORE the
IO operation (count = `Ix`/`Iy`/`Ox`/`Oy`), then `iCycleCount` is reset
to a small post-IO amount (`Ix_=0, Iy_=0, Ox_=4, Oy_=4`) and the IO
runs. The total matches 1984's single-chunk count, but the **CRTC tick
and IRQ-acceptance check land at a different point inside the
instruction**.

For example, OUT (n),A in konCePCja:
```cpp
case outa: { z80_wait_states iCycleCount = Oa_;} z80_OUT_handler(...); break;
//          ^^^^^^^^^^^^^^^^^ — wait_states sees iCycleCount=Oa=8
//                              then resets to Oa_=4 for the IO chunk
```

1984's OUT (n),A returns Oa = 8 cycles total. The CRTC/GA advance once
per instruction, after the IO. konCePCja's advances mid-instruction.

For an HDCPM disk-scan loop hammering IDE ports, the per-OUT mismatch
in CRTC tick timing accumulates and could plausibly land an IRQ
mid-instruction differently in the two emulators.

**Fixing this requires restructuring `z80_step()` to invoke the bus
arbiter between instruction sub-phases**, not just at the end. It's
a real refactor — bigger than a one-line cycle-table patch.

### 3. Speed delta still ~80% slower than konCePCja

konCePCja reaches `A0:CPM>` in ~44 s. 1984 (with current fixes)
reaches it in ~80 s when it succeeds. The remaining 36 seconds of
extra emulator time has to come from somewhere — either many
instructions running slightly slow, or a few hot loops running
substantially slow.

A `ONE_K_TRACE_IM1` IRQ count comparison between the two would pin
down whether 1984 is running fewer T-states *per IRQ accept* (matches
konCePCja) but emitting *more IRQs* per CPC second (clock-rate
mismatch), or running *more T-states per accept* (instruction-level
slowdown still present).

## Recommended next session

1. **Compare IRQ counts at equivalent CPC time**: capture IRQ totals
   from both emulators at, say, +30 s into a known-good boot. If 1984
   has 30 % more IRQs than konCePCja, the extra cost is in instruction
   cycles, not in IRQ overhead.
2. **Trace LD I,A occurrences**: instrument `src/z80.c` to log every
   ED 47 / ED 4F execution with `frame`, `PC`, and `IFF1`. Compare
   counts in the failing path with konCePCja's count (via its IPC
   `step trace` + filter). If 1984 sees a different number of
   executions, the regression has a structural explanation.
3. **Audit `z80_step()` for instruction-mid bus activity**: even just
   inserting a `crtc_tick(4); ga_tick(); ppi_tick();` between FETCH
   and IO for OUT (n),A could close part of the gap. Risky but
   straightforward.

## Files for this session

- `koncepcja-at-30s.png` / `-50s.png` — konCePCja headless captures
  showing it boots to A0:CPM> by ~50 s.
- `CRASH-PINPOINTED.md` — original 1984 crash signature.
- `PER-FRAME-CPU-EVOLUTION.md` — frames 3265–3270 CPU state pre/post.
- `KONCEPCJA-SURVIVES.md` — proof the same workload survives in
  konCePCja.
- `CYCLE-DIFF-FINDINGS.md` (this file).

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

### 3. ~~Speed delta still ~80% slower than konCePCja~~ (RESOLVED — apparent only)

Initial reading of the konCePCja-vs-1984 boot timings (44 s vs 80 s
wall-clock to A0:CPM>) suggested a major remaining cycle deficit. That
was an apples-to-oranges comparison: **konCePCja in headless mode
free-runs the Z80** (`if (CPC.limit_speed && ...) sleep` at
`kon_cpc_ja.cpp:3626`), only pacing when the speed limit is enabled.
1984's main loop always paces to 50 fps via `SDL_DelayNS`
(`src/main.c:1032-1036`). So konCePCja's headless ran the same
emulator workload faster in **wall-clock** but identical in **emulator
T-states**. Both reach A0:CPM> in roughly the same emulator-time;
1984's effective cycle count is close to konCePCja's.

The remaining instability is therefore not accumulated cycle drift but
a specific cycle-alignment race that still fires occasionally inside
the CP/M+ "Scanning startup devices" path.

## Recommended next session

1. **Add `crtc_tick`/`ga_tick` calls inside IO instructions** to mirror
   konCePCja's `z80_wait_states`-then-IO-then-`z80_wait_states` split.
   The CRTC tick advance ratio (4 T-states per tick) means landing the
   IRQ-acceptance check at a different T-state inside an OUT (n),A
   could change the GA interrupt counter state at the next instruction
   boundary — which is exactly the kind of fine-grained alignment that
   triggers the residual #129 race.
2. **Trace LD I,A occurrences**: instrument `src/z80.c` to log every
   ED 47 / ED 4F execution with `frame`, `PC`, and `IFF1`. Compare
   counts in the failing path with konCePCja's count (via its IPC
   `step trace` + filter). If 1984 sees a different number of
   executions, the LD I,A regression has a structural explanation.
3. ~~Bisect block-IO patterns by bumping cc_op for OUT~~ (TRIED,
   regresses). Setting `Oa=12, Ox=12, Oy=16` to match konCePCja's
   split-totals corrupted PSG / keyboard-matrix timing — at frame 4000
   the screen shows BASIC Ready with garbled keystrokes typed in
   (random arrow / cursor characters mixed into the line buffer), then
   `|hdcpm` → "Syntax error". So the per-OUT cycle delta CAN'T just be
   collapsed into the table — it has to interleave with the bus arbiter
   the way konCePCja does. The right fix is a real `z80_step()`
   restructure to split each IO into "pre-cycles | IO | post-cycles"
   with `cpc_frame()`'s per-cycle hooks called between segments.

## Files for this session

- `koncepcja-at-30s.png` / `-50s.png` — konCePCja headless captures
  showing it boots to A0:CPM> by ~50 s.
- `CRASH-PINPOINTED.md` — original 1984 crash signature.
- `PER-FRAME-CPU-EVOLUTION.md` — frames 3265–3270 CPU state pre/post.
- `KONCEPCJA-SURVIVES.md` — proof the same workload survives in
  konCePCja.
- `CYCLE-DIFF-FINDINGS.md` (this file).

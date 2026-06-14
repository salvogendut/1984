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

## Status after the bus-tick infrastructure shipped

Commits `2355537..5fa5197` on the branch implement the full IO-cycle
split sketched above. Z80Bus has the `tick(N)` + `ticked_in_step`
fields. cpc.c has `cpc_advance_crtc()` extracted from `cpc_frame()`
and a `cpc_bus_tick()` callback wired in. OUT (n),A, OUT (C),r,
OUTI/OUTD/OTIR/OTDR, and the IM1/IM2 IRQ accept all call
`bus->tick(N)` mid-instruction now.

Result on the #129 race: **no improvement**. The infrastructure is
correct (boot still reaches CP/M Plus, no regressions), but moving
the bus-arbiter call earlier inside IO instructions and IRQ accept
doesn't close the cold-reset window. The race fires the same way
whether the bus advance is all-after-instruction (old) or split
pre-IO/post-IO (new).

This is moderately strong evidence that the race is **not** in
GA-interrupt-counter alignment relative to IO instructions. Two
other categories remain:

- **Per-instruction T-state errors** beyond the iWSAdjust set —
  individual opcodes might be 1–2 cycles off in their cc_op table
  values vs konCePCja, accumulating drift over the long CP/M+ scan
  loop. Cross-checking the cc_op/cc_cb/cc_ed/cc_xy tables byte-for-
  byte against konCePCja showed them IDENTICAL, but konCePCja's
  IMPL paths use `_` constants (the post-IO portion) that 1984
  doesn't apply — and we've shown bumping them to match regresses.
- **Memory-banking timing** inside CP/M+'s upper-bank dance —
  the kernel switches between banks 0/4/5/6/7 at 0xC000-0xFFFF via
  the `0x7Fxx` MMR write; if 1984's bank-select takes effect at a
  different T-state than konCePCja, the CPU could fetch from the
  wrong bank for one cycle and execute a wrong instruction.

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
   regresses two ways):
   - Setting all three (`Oa=12, Ox=12, Oy=16`) at once corrupted
     PSG / keyboard-matrix timing — frame 4000 shows BASIC Ready with
     garbled keystrokes (arrow chars + `|hdcpm` → "Syntax error").
   - Setting just `Oa=12` (OUT (n),A) alone — to target HDCPM's IDE
     port writes specifically — also failed: cold-reset back to BASIC
     by frame 4500 with no CP/M+ at all.

   Both confirm the same root cause: the per-IO cycle delta CAN'T be
   collapsed into the cc_op table. konCePCja's `z80_wait_states`
   processes the **pre-IO** cycles, lets the bus update (CRTC tick,
   IRQ counter, PSG, etc.) **before** the IO completes, then accounts
   for **post-IO** cycles separately. 1984 lumps everything into one
   chunk after the IO returns. The right fix is to add a tick(N)
   callback to `Z80Bus` and have `z80_step_impl()` call it inside IO
   ops, mirroring konCePCja's split-then-IO-then-split dance.

   Sketch:
   ```c
   typedef struct {
       ... existing fields ...
       void (*tick)(void *ctx, int cycles);   /* new: bus arbiter */
       int  *ticked_in_step;                  /* zeroed per z80_step */
   } Z80Bus;

   /* In z80_step (wrapper), zero *ticked_in_step at entry. */
   /* In z80_step_impl(), for OUT (n),A: */
   case 0xD3: {
       u8 n = FETCH8();                /* 4-cycle fetch */
       bus->tick(bus->ctx, 8);         /* pre-IO bus arbiter (Oa) */
       OUT((cpu->a<<8)|n, cpu->a);     /* IO write */
       return 12;                      /* total cycles */
   }

   /* cpc_frame() main loop, after z80_step returns t cycles: */
   int remaining = t - cpc->bus_ticked_in_step;
   if (remaining > 0) cpc_advance_bus(cpc, remaining);
   ```

   **Refactor prerequisite identified this session**: the CRTC tick
   loop in `cpc_frame()` uses three function-scope locals —
   `cur_ma`, `cur_ra`, `cur_de` (cpc.c:746-748) — captured before
   the step and updated at each char-clock tick (cpc.c:1121-1123).
   For a mid-instruction `bus->tick(N)` to work, those locals have
   to move into the `CPC` struct so the second chunk of CRTC work
   resumes from the state the first chunk left in them. Without
   that move, the second chunk would still see the pre-step values
   and render incorrect addresses. This is a contained ~30-line
   refactor of `cpc_frame()` that must precede the Z80Bus tick hook.

## Files for this session

- `koncepcja-at-30s.png` / `-50s.png` — konCePCja headless captures
  showing it boots to A0:CPM> by ~50 s.
- `CRASH-PINPOINTED.md` — original 1984 crash signature.
- `PER-FRAME-CPU-EVOLUTION.md` — frames 3265–3270 CPU state pre/post.
- `KONCEPCJA-SURVIVES.md` — proof the same workload survives in
  konCePCja.
- `CYCLE-DIFF-FINDINGS.md` (this file).

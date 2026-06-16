# Issue #62 — FUZIX support

FUZIX boots all the way to the multi-user login on the `62-fuzix` branch. Enter `hda1` at the `bootdev:` prompt, log in as `root` — full shell.

## Target

**ajcasado's FUZIX port for the CPC 6128**, v0.5.1-alpha3 (Jul 2025), platform `cpcsme` (CPC with Standard Memory Expansion, thunked 64K-block memory model up to 1024 KB).

- Upstream: https://github.com/ajcasado/FUZIX
- Source: `Kernel/platform/platform-cpcsme/` + `Kernel/dev/cpc/`
- Release: https://github.com/ajcasado/FUZIX/releases/tag/v0.5.1-alpha3

Release assets (mirrored to `~/Downloads/fuzix-cpc/`):

| File | Size | Purpose |
|---|---|---|
| `fuzixcpc.sna` | 130 KB | Snapshot — fastest path to a booted kernel, 128 KB CPC state |
| `fuzix.dsk` | 200 KB | Boot floppy — load and `RUN"FUZIX` from BASIC |
| `root.dsk` | 760 KB | Minimal root filesystem on drive B |
| `disk.img` | 64 MB | IDE / X-Mass mass-storage image (contains `hda1` rootfs) |
| `blankA.dsk` | 190 KB | Empty drive A |

## How to run

Minimal config: CPC 6128, 512 KB RAM, SymbIface IDE enabled with `disk.img`. The shipped `/tmp/fuzix-test.conf` has it.

```bash
./1984 --config=/tmp/fuzix-test.conf \
  --disk-a=~/Downloads/fuzix-cpc/fuzix.dsk \
  --disk-b=~/Downloads/fuzix-cpc/root.dsk \
  --autostart=fuzix
```

At `bootdev:` type `hda1` ⏎. Then at `login:` type `root` ⏎ — no password required by default.

128 KB is not enough; the kernel needs ≥256 KB usable, and ≥512 KB total comfortably hosts six processes. The `fuzixcpc.sna` snapshot is hard-coded to 128 KB and prints `WARNING: Increase PTABSIZE` — prefer the floppy boot path.

## Fixes that landed during bring-up

| Commit | Bug | Why FUZIX hit it |
|---|---|---|
| `1a3393a` | FDC port decode now requires `lo bit 7 = 0` | Real FDC lives at `0xFB7E/0xFB7F`; we used to claim the whole `0xFB**` range. FUZIX's Usifac probe at `0xFBD8` was reading the FDC main status register instead of `0xFF`, the driver thought a Usifac chip was present, and hung in `usifac_flush()`. |
| `a3877ee` | CRTC port decode now also requires `A9=0` for the write side | Real CPC CRTC chip-select is `A14=0`; `A9` selects function: `A9=0,A8=0` → select (`BCxx`), `A9=0,A8=1` → write (`BDxx`), `A9=1` → read side (`BExx/BFxx`). Our old decode latched any `A14=0,A8=1` port as a write. FUZIX's SDCC port helper at kernel `F995` issues `OUT (C),B` with `BC=0x03FF` (`A14=0,A9=1,A8=1` — read port) for unrelated bus work; on real hardware the CRTC ignores it, but we clobbered `R12 := 0x03`, pointed the screen at block 0, and produced the band-of-pixels-per-text-row corruption captured in `1984-20260616-153532.gif`. |

## What we cross-checked along the way (still useful as reference)

FUZIX's `Kernel/platform/platform-cpcsme/cpcsme.s` programs the CRTC for its console once, at `init_hardware`, then never touches R0/R3/R4/R5/R9 again:

```asm
ld bc,#0xbc01 / out (c),c / ld bc,#0xbd20 / out (c),c   ; R1 = 0x20
ld bc,#0xbc02 / out (c),c / ld bc,#0xbd2A / out (c),c   ; R2 = 0x2A
ld bc,#0xbc06 / out (c),c / ld bc,#0xbd20 / out (c),c   ; R6 = 0x20
ld bc,#0xbc07 / out (c),c / ld bc,#0xbd22 / out (c),c   ; R7 = 0x22
ld bc,#0xbc0c / out (c),c / ld bc,#0xbd10 / out (c),c   ; R12 = 0x10
```

The displayed area is 32 char clocks × 32 char rows = 512×256 px; FUZIX calls it "64×32" referring to 8×8 font characters (two per char clock). VRAM bases are `0x4000` (tty1) and `0x8000` (tty2). Gate Array MMR mode `0xC1` keeps blocks 0/1/2 at their natural Z80 addresses, swaps block 7 in at the top:

```
0x0000-0x3FFF : block 0 (vectors)
0x4000-0x7FFF : block 1 (VRAM tty1)
0x8000-0xBFFF : block 2 (VRAM tty2)
0xC000-0xFFFF : block 7 (kernel common)
```

In all GA modes the CRTC reads only the base 64 KB (blocks 0-3), so Z80 writes at `0x4000` in mode `0xC1` and CRTC reads at `0x4000` land on the same byte.

Scrolling is via `R12/R13` from `cpc_scroll_up`/`cpc_scroll_down` in `Kernel/dev/cpc/videosme.s`. The screenpage byte (`0x10` for tty1, `0x20` for tty2) is OR-ed into the high byte of `R12` so the screen always stays in the right block; that math is fine.

## Debug hooks added during this work (gated by master debug switch)

- `DUMP_VIDEO_RAM=/path/file` — once per frame, writes the entire physical RAM followed by the 18 CRTC registers. Last frame wins; quit the emulator on the corrupted state and the file captures it.
- `ONE_K_TRACE_CRTC_REGS=1` — `[CRTC] PC=… R… = 0x… BC=… HL=… DE=… AF=…` for every CRTC register write.

Both go through `dbg_getenv()` so they cost nothing when `cfg.debug` is off.

## Open follow-ups (not blocking)

- The `fuzixcpc.sna` snapshot is 128 KB — works but prints the PTABSIZE warning. Cosmetic.
- Floppy-based boot needs `--memory=512`; surfacing this in the UI for 6128 + FUZIX users would be nice.
- Keyboard input at the `bootdev:` and `login:` prompts works via our PPI/PSG path with no special FUZIX accommodation — but the FUZIX keymap is US-layout, so non-US users typing punctuation will see the usual CPC US mapping.

# Issue #62 — FUZIX port progress notes

Working notes from the first session on the FUZIX bring-up. Branch: `62-fuzix`.

## Target

**ajcasado's FUZIX port for the CPC 6128**, v0.5.1-alpha3 (Jul 2025), platform name `cpcsme` (CPC with Standard Memory Expansion, thunked 64K-block memory model up to 1024 KB).

- Upstream: https://github.com/ajcasado/FUZIX
- Source: `Kernel/platform/platform-cpcsme/` + `Kernel/dev/cpc/`
- Release: https://github.com/ajcasado/FUZIX/releases/tag/v0.5.1-alpha3

Release assets (we have all five mirrored to `~/Downloads/fuzix-cpc/`):

| File | Size | Purpose |
|---|---|---|
| `fuzixcpc.sna` | 130 KB | Snapshot — fastest path to a booted kernel, 128 KB CPC state |
| `fuzix.dsk` | 200 KB | Boot floppy — load and `RUN"FUZIX` from BASIC |
| `root.dsk` | 760 KB | Minimal root filesystem on drive B |
| `disk.img` | 64 MB | IDE / X-Mass mass-storage image |
| `blankA.dsk` | 190 KB | Empty drive A |

## What works today on `62-fuzix`

| Path | Behavior |
|---|---|
| `--load-sna=fuzixcpc.sna` | Loads cleanly. Snapshot is hard-coded to 128 KB so the kernel prints `WARNING: Increase PTABSIZE to 256 to use available RAM`. |
| `--disk-a=fuzix.dsk --disk-b=root.dsk --autostart=fuzix --memory=512` | Boots through the FUZIX splash (cat + rainbow), kernel banner, copyright lines, RAM probe ("512 KiB total RAM, 360 KiB available to processes (6 processes max)"), `Enabling interrupts ... ok`, `Configuring Usifac` → `Usifac not present`. |
| `cfg.symbiface_ide=true` + `cfg.ide_image=disk.img` | Both IDE drives detected. FUZIX prints `0000 : 1984 IDE Drive - OK` / `0001 : 1984 IDE Drive - OK`, then `hda: hda1 hda2 hda3 hda4` + `hdb: hdb1 hdb2 hdb3 hdb4`. **Our SymbIface II IDE emulation is compatible with the FUZIX driver out of the box.** |

## What's blocking the boot

After the IDE probe FUZIX **switches the CRTC into a non-standard 32 char-clock × 32 char-row layout** for its interactive console. From `Kernel/platform/platform-cpcsme/cpcsme.s` ~line 353:

```asm
; we set the crtc for a screen with 64x32 colsxrows, video page at 0x4000
; https://www.cpcwiki.eu/index.php/CRTC
ld bc,#0xbc01
out (c),c          ; CRTC select R1
ld bc,#0xbd20
out (c),c          ; R1 = 0x20  (32 char clocks displayed horizontally)
ld bc,#0xbc02
out (c),c          ; R2
ld bc,#0xbd2A
out (c),c          ; R2 = 0x2A  (hsync position 42)
ld bc,#0xbc06
out (c),c          ; R6
ld bc,#0xbd20
out (c),c          ; R6 = 0x20  (32 char rows displayed)
ld bc,#0xbc07
out (c),c          ; R7
ld bc,#0xbd22
out (c),c          ; R7 = 0x22  (vsync position 34)
ld bc,#0xbc0c
out (c),c          ; R12
ld bc,#0xbd10
out (c),c          ; R12 = 0x10 (screen base = 0x4000)
```

The kernel "64×32" comment refers to **font characters** (8×8 font, two font chars per 16-pixel char clock); the CRTC actually displays 32 char clocks × 32 char rows = 512×256 pixels. Memory map is the entire 16 KB at `0x4000-0x7FFF`.

The Gate Array is simultaneously switched to MMR mode `0xC1` (`ld bc,#0x7fc1; out (c),c` — see `map_video` in `cpcsme.s`):

```
0x0000-0x3FFF : bank 0  (vectors)
0x4000-0x7FFF : bank 1  (VRAM tty1)
0x8000-0xBFFF : bank 2  (VRAM tty2)
0xC000-0xFFFF : bank 7  (kernel common)
```

After the switch, our renderer produces visible content but it's not legible:
- A solid white horizontal line at the cursor row (FUZIX writes `0xFF` for the cursor — `cpc_cursor_on` in `Kernel/dev/cpc/videosme.s`).
- Bands of partial pixels at ~8-line intervals — looks like only 1–2 of the 8 scan lines per char row are being rendered, with border filling the rest.

## What we've confirmed about our CRTC

Cross-checked against `/var/home/salvogendut/Dev/caprice32/src/crtc.cpp` and `/var/home/salvogendut/Dev/konCePCja/src/crtc.cpp`:

- **Pixel-fetch address formula matches both reference emulators byte-for-byte.** Caprice32 / konCePCja use `MAXlate[l] = (j & 0x7FE) | ((j & 0x6000) << 1)` where `j = MA << 1`; our `src/cpc.c` computes the same as `(bank << 14) | ((RA & 7) << 11) | (col << 1)` with `bank = MA[13:12]`, `col = MA[9:0]`.
- **R0/R1/R6/R7/R12 handling.** All used in `crtc_tick` / `display_enable`. MA reload at frame start, MA row advance by R1, vlc/vcc tracking — all match the documented CRTC behaviour.
- **HSync edge detection.** `raster_y++` and `raster_x = RASTER_X_AFTER_HSYNC` happen at the hsync falling edge; matches Caprice32's scan-line advance trigger.

## Open hypotheses (not yet proven)

1. **`crtc_pre_ra` staleness during bus-tick interleaving.** Our `cpc_advance_bus` is called both from `cpc_frame` and from the Z80's `bus.tick` callback (mid-instruction). The renderer reads `crtc_pre_ra` which is captured at the *end* of the previous char-clock tick. If the bus-tick advance crosses a scan-line boundary (where `vlc` increments), the renderer for the next char clock could fetch with an RA from the wrong scan line. The standard CRTC config might mask this because the timing arithmetic happens to land on safe ticks; FUZIX's R0=63/R2=42/R6=32 layout doesn't.

2. **`RASTER_X_AFTER_HSYNC` calibration.** Hard-coded at `-1`, calibrated for default `R2+R3=60`. With FUZIX's `R2+R3=56`, displayed area lands at px 112-608 instead of 64-704. Not corruption per se, but content shifts within the framebuffer. Probably not the cause of the visible breakage on its own.

3. **Display-enable lag.** The renderer uses `crtc_pre_de` (display_enable from previous tick). With FUZIX's narrow R1=32, this could produce a 1-char-clock smear at the right edge of the displayed area. Not enough to explain the broad pattern we see.

## Reproducing the breakage

```bash
# In ~/.config/1984/1984.conf put model=6128, memory=512,
# symbiface_ide=true, ide_image=~/Downloads/fuzix-cpc/disk.img.
# Or copy /tmp/fuzix-test.conf which has the right values.

./1984 --config=/tmp/fuzix-test.conf \
  --disk-a=~/Downloads/fuzix-cpc/fuzix.dsk \
  --disk-b=~/Downloads/fuzix-cpc/root.dsk \
  --autostart=fuzix
```

Kernel banner appears in standard 40×25 layout (works), enumerates devices (works), corruption starts the frame after the CRTC switch (~frame 534 in the user's recorded GIF `1984-20260616-153532.gif`).

## Proper next-step plan

Don't keep guessing. The right way to find the renderer bug:

1. **Add a one-shot CRTC trace.** New env var `ONE_K_TRACE_CRTC=N` that for N frames logs every (frame, hcc, vcc, vlc, MA, display_enable) at every `crtc_tick` exit. Costs nothing when off.
2. **Add a one-shot pixel-fetch trace.** Log every (frame, raster_y, raster_x, addr, b0, b1) during the same window.
3. **Capture the same `.sna` in Caprice32 with its existing debug hooks.** Diff the trace lines from the same instruction-level checkpoint in both emulators. The first divergent address / scan-line tells us exactly where to look.
4. **Fix one thing at a time, retest with the standard CPC firmware** to make sure we don't regress BASIC / SymbOS / HDCPM.

That's a focused half-to-full day of work. Resume in a fresh session with no scrollback noise.

## Misc artifacts from this session

- **FDC port-decode fix committed** as `1a3393a` (gates read decode on `lo bit 7 = 0`). Real bug that affected any peripheral occupying `0xFB80–0xFBFF`. Without this, FUZIX's `usifexists` probe at `0xFBD8` read the FDC main status register instead of the expected `0xFF`, so the driver thought a Usifac was present and hung in `usifac_flush()` polling for a chip that isn't there.
- `/tmp/fuzix-test.conf` — minimal test config (model 6128, 512 KB, IDE image set, no peripherals beyond the SymbIface).
- User's GIF capture of the boot: `1984-20260616-153532.gif` (602 frames @ ~50 Hz).
- The FUZIX kernel itself is fine; the boot is sitting at the `boot device?` prompt waiting for keyboard input we just can't see.

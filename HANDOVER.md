# CPC Plus support handover

## Scope and branch

Work is on `issue-237-cpc-plus` for GitHub issue #237. The branch adds CPC
464 Plus and CPC 6128 Plus machines and advances their ASIC/video behavior far
enough to boot the system cartridge and run demanding cartridge software.

Published branch commits at the time of this handover:

- `5827bad` - Add CPC Plus machine and cartridge support
- `adad579` - Fix advanced CPC Plus timing for Sonic GX
- `234920d` - Fix CPC Plus split and scrolling video addressing
- `dad8255` - Fix CPC Plus split priority during fine scroll

There may be newer uncommitted work after those commits. Always inspect
`git status` and `git diff` before continuing.

## User-facing operation

The machine models are available in the General overlay and on the command
line:

```sh
./1984 --464plus
./1984 --6128plus
./1984 --6128plus --cartridge=/path/to/game.cpr
```

The cartridge picker is under **Media > Cartridge** for Plus machines. The
bundled `roms/system.cpr` is the default cartridge.

CRTC selection is exposed under **Advanced > CRTC type** and as:

```ini
[machine]
crtc=auto
```

Accepted values are `auto`, `type0`, `type1`, `type2`, and `type3`; the same
values are accepted by `--crtc=TYPE`. Auto selects Type 3 for Plus, Type 1 for
the CPC 6128, and Type 0 for CPC 464/664. The setting is retained across
reset, and Delete on the overlay entry restores Auto.

## Implemented hardware

### Cartridge and memory mapping

- RIFF CPR parsing with sparse or out-of-order `cb00` through `cb31` chunks.
- System and game cartridge lower-ROM mapping.
- Plus upper-ROM selection and RMR2 lower-page/bank selection.
- ASIC register page mapping at `0x4000-0x7fff` while unlocked and selected.
- Separate 464 Plus and 6128 Plus RAM, FDC, and cassette defaults.

### ASIC access and video

- Documented ASIC lock/unlock sequence.
- 32-entry 12-bit Plus palette with raster-time updates.
- Sixteen 16x16 hardware sprites, magnification, priority, coordinate
  wrapping, and mid-frame data/attribute changes.
- Plus mode pixel decoding and raster-time mode changes.
- SSCR horizontal and vertical fine scrolling.
- SSSL/SSA screen splits, including multiple splits in one frame and split
  priority when vertical scrolling crosses a CRTC row.
- Video addressing based on the programmed CRTC R1 row width rather than a
  fixed 40-character firmware width.
- Monitor/CRTC timing improvements needed by overscan and changing screen
  geometry.

### Interrupts and audio DMA

- Programmable raster interrupts sampled from CRTC counters.
- ASIC IM2 vectors, raster/DMA source priority, and DCSR acknowledgement.
- Three PSG DMA channels with LOAD, PAUSE, REPEAT, LOOP, interrupt, stop, and
  prescaler behavior.
- Z80 interrupt acknowledge timing needed by Sonic GX.

## Important fixes and findings

Sonic GX exposed two independent addressing issues. Vertical fine scroll was
advancing by a hard-coded firmware row width instead of R1, and an active
screen split had to translate the independently running CRTC address rather
than overwrite the CRTC's MA counters. Split address loading also has priority
over the fine-scroll row advance.

The Ghosts 'n Goblins preview exposed a false ASIC comparator match. PRI and
SSSL program a five-bit character-row value, but the live CRTC VCC is wider.
The old code masked VCC with `0x1f`; for example, VCC 34/RA 2 incorrectly
matched programmed line `0x12` (VCC 2/RA 2). The resulting false split near
the end of the frame selected executable RAM as video memory, producing the
large striped/garbled field while hardware sprites remained coherent. Rows
32 and above must not wrap into a comparator match. Regression tests cover
both PRI and SSSL.

DCSR bit 7 is the source of the last interrupt acknowledgement, not the live
programmable-raster request. Raster acknowledgement sets it and DMA
acknowledgement clears it.

## Regression media

- `roms/system.cpr`: system UI and Burning Rubber.
- `/var/home/salvogendut/Downloads/Sonic_the_Hedgehog__ENGLISH.cpr`:
  intro split, scrolling gameplay, IM2 interrupts, sprites, and fine scroll.
- `/var/home/salvogendut/Downloads/Ghosts_n_Goblins_v0.11b__(2020-03-04)__(Release_GOLEM13)__PUBLIC_PREVIEW/Ghosts_n_Goblins_v0.11b__(2020-03-04)__(Release_GOLEM13)__PUBLIC_PREVIEW.cpr`:
  multiple changing splits and late-frame comparator behavior.

Automated Ghosts replay:

```sh
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./1984 \
  --config=/dev/null --6128plus \
  --cartridge='/path/to/Ghosts_n_Goblins_preview.cpr' \
  --joy-script=-:600,1:5,-:395 \
  --gif-out=/tmp/ghosts.gif \
  --screenshot-at=900:/tmp/ghosts-900.ppm \
  --exit-after=1000
```

At frame 900 the expected result is a coherent HUD, mountain background,
graveyard terrain, player, and enemies. The former result was a full field of
horizontal code-like stripes with only the hardware sprites recognizable.

## Build and tests

For a fresh checkout:

```sh
autoreconf -iv
./configure
make -j4
make -C tests check
```

Useful focused tests are `tests/test-asic`, `tests/test-cartridge`,
`tests/test-crtc`, `tests/test-mem`, and `tests/test-z80`.

## References

The implementation was compared primarily with Caprice32, with Sugarbox used
for Plus-specific behavior not reproduced correctly by the local Caprice32
build. The most useful external hardware reference was the Extra CPC Plus
Hardware Information document at `https://cpctech.cpcwiki.de/docs/cpcplus.html`.

## Known gaps and follow-up work

- CRTC Type 3 is selectable and is the Plus Auto default, but undocumented
  edge cases still need cartridge-by-cartridge conformance testing.
- Plus ASIC internals are not fully serialized by SNA snapshots. A restored
  snapshot may need the cartridge code to rebuild palette, sprite, split,
  interrupt, and DMA state.
- SSCR extend-border behavior and unusual mode/border combinations need more
  focused tests.
- Continue testing system/Burning Rubber and Sonic whenever split, CRTC, or
  monitor timing changes; fixes for one cartridge can expose another timing
  assumption.
- Keep diagnostic trace hooks temporary. Do not leave high-frequency raster
  logging in production paths.

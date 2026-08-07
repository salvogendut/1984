# CPC Plus support

1984 supports the CPC 464 Plus and CPC 6128 Plus as separate machine models.
Both boot the bundled Amstrad v4 system cartridge (`roms/system.cpr`), and a
standalone RIFF CPR can be selected from **Media > Cartridge** or supplied
on the command line:

```sh
./1984 --6128plus --cartridge=/path/to/game.cpr
./1984 --464plus --cartridge=/path/to/game.cpr
```

The CPR reader accepts sparse and out-of-order `cb00` through `cb31` chunks.
The Plus memory path implements the system-cartridge lower and upper ROM
layout, RMR2 bank selection, and the ASIC register page at `0x4000-0x7fff`.

The initial ASIC implementation includes:

- the documented lock/unlock sequence and RMR2 mapping;
- the 32-entry 12-bit palette;
- sixteen 16x16 hardware sprites with magnification and priority;
- horizontal and vertical soft scrolling;
- programmable raster interrupts and screen splits;
- three PSG DMA channels with pause, repeat, loop, interrupt, and stop;
- Plus-aware 64 KB/128 KB, cassette, and floppy model defaults.

Caprice32 is the behavioral reference. The v4 system cartridge, 1942, and
Robocop 2 are current smoke/regression cartridges. Exact Plus CRTC type-3
edge cases, raster-time sprite register changes, and ASIC IM2 interrupt-vector
delivery remain fidelity work; the normal CPC CRTC path is intentionally used
for the current Plus profile because it boots the system cartridge reliably.

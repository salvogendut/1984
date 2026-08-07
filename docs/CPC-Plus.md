# CPC Plus support

1984 supports the CPC 464 Plus and CPC 6128 Plus as separate machine models.
Both boot the bundled Amstrad v4 system cartridge (`roms/system.cpr`), and a
standalone RIFF CPR can be selected from **Media > Cartridge** or supplied
on the command line:

```sh
./1984 --6128plus --cartridge=/path/to/game.cpr
./1984 --464plus --cartridge=/path/to/game.cpr
```

The CRTC can be selected under **Advanced > CRTC type**, in the configuration
file with `crtc=auto|type0|type1|type2|type3`, or on the command line with
`--crtc=TYPE`. `Auto` selects Type 3 for Plus machines, Type 1 for the CPC
6128, and Type 0 for the CPC 464/664. Pressing Delete on the Advanced entry
restores `Auto`.

The CPR reader accepts sparse and out-of-order `cb00` through `cb31` chunks.
The Plus memory path implements the system-cartridge lower and upper ROM
layout, RMR2 bank selection, and the ASIC register page at `0x4000-0x7fff`.

The initial ASIC implementation includes:

- the documented lock/unlock sequence and RMR2 mapping;
- the 32-entry 12-bit palette;
- sixteen 16x16 hardware sprites with magnification, priority, coordinate
  wrapping, and raster-time register updates;
- horizontal and vertical soft scrolling;
- CRTC-derived programmable raster interrupts and screen splits;
- ASIC interrupt-mode-2 vectors and source acknowledgement;
- three PSG DMA channels with pause/prescaler timing, repeat, loop, interrupt,
  and stop;
- Plus-aware 64 KB/128 KB, cassette, and floppy model defaults.

Caprice32 is the general behavioral reference, with Sugarbox used where its
Plus behavior is more complete. The v4 system cartridge, 1942, Robocop 2,
Sonic GX, and the Ghosts 'n Goblins preview are current smoke/regression
cartridges. Sonic GX also exercises ASIC IM2 interrupt vectors. Exact CRTC
edge cases and snapshot serialization of internal ASIC state remain fidelity
work.

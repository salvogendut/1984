# CPC implementation cross-check

Issue: #266

Reference revision: floooh/chips `afc331442ce28bf455bee7d81bc0b0c52ebf833d`

Primary references:

- [systems/cpc.h](https://github.com/floooh/chips/blob/afc331442ce28bf455bee7d81bc0b0c52ebf833d/systems/cpc.h)
- [chips/am40010.h](https://github.com/floooh/chips/blob/afc331442ce28bf455bee7d81bc0b0c52ebf833d/chips/am40010.h)
- [chips/i8255.h](https://github.com/floooh/chips/blob/afc331442ce28bf455bee7d81bc0b0c52ebf833d/chips/i8255.h)
- [chips/ay38910.h](https://github.com/floooh/chips/blob/afc331442ce28bf455bee7d81bc0b0c52ebf833d/chips/ay38910.h)
- [chips/mc6845.h](https://github.com/floooh/chips/blob/afc331442ce28bf455bee7d81bc0b0c52ebf833d/chips/mc6845.h)
- [chips/upd765.h](https://github.com/floooh/chips/blob/afc331442ce28bf455bee7d81bc0b0c52ebf833d/chips/upd765.h)

The comparison is behavioral. `chips` is a useful independent pin-level model,
but it is not a complete golden implementation: it has one CRTC type, one disk
drive, incomplete cassette support, and no CPC Plus or GX4000 support.

## Confirmed matches

| Area | Result |
| --- | --- |
| Master clocks | Both use a 4 MHz Z80 and 1 MHz CRTC/AY clock domain. |
| 6128 RAM banking | All eight four-page mappings match exactly. 1984 additionally supports expansion groups and larger RAM. |
| ROM overlays | Lower and upper ROMs overlay reads while writes continue to the RAM behind them. |
| Video RAM | CRTC video fetches use the physical base 64 KB, independent of CPU RAM banking. |
| Core selects | CRTC `A14=0`, PPI `A11=0`, Gate Array `A15=0/A14=1`, and printer `A12=0` agree. |
| Keyboard/joystick | PPI port C selects a matrix row and AY port A returns the active-low row; joystick 1 shares row 9. |
| Gate Array IRQ | The 52-HSYNC counter, two-HSYNC VSYNC resynchronization, IRQ reset, and acknowledge clearing counter bit 5 agree. |
| CRTC addressing | The displayed address combines `MA13..12`, `RA2..0`, `MA9..0`, and the byte phase in the same order. |
| FDC registers | The conventional `FAxx` motor and `FBxx` status/data behavior agrees with Caprice32 and 1984. |

## Defects corrected on this branch

### Simultaneous I/O selection

1984 previously used an `if/else if` read dispatcher and early returns on
writes. Real CPC partial decoding allows several onboard devices to observe one
I/O cycle. The important case documented by `chips` is Arnold Acid `OnlyInc`:
the PPI drives the data bus first, then an overlapping Gate Array selection
consumes that value even though the CPU executed `IN`.

The dispatcher is now compositional. PPI, Gate Array, ROM latch, CRTC, and
printer selects are represented independently in `src/io_decode.h`. Gate Array
register writes occur on both CPU reads and writes, with the PPI evaluated
first. CRTC write-side aliases reached through `IN` also consume the current bus
value because CRTC R/W is wired to address line A9 rather than Z80 RD/WR.

### Upper-ROM latch aliases

1984 accepted ROM selection only at `C0xx-DFxx`. The hardware latch only checks
`A13=0` during a write. All corresponding aliases now select the upper ROM and
can overlap other onboard devices.

### 8255 direction and reset behavior

The PPI was reset directly to firmware mode `0x82`, ignored port direction on
reads and writes, and returned `0xFF` for the control register. It now resets to
the 8255 hardware state `0x9B` (all ports input), gates A/B latches by direction,
combines port C output halves correctly, and returns the control word. The CPC
firmware still programs `0x82` during normal boot.

### AY register and port-A reads

AY register 7 was incorrectly masked to six bits, deleting the port-A direction
bit. PPI reads also always returned keyboard data rather than the selected AY
register. Register 7 now retains its I/O direction bits. AY registers read back
normally, while register 14 reads the keyboard only when port A is configured as
input and reads its latch when configured as output.

### Reset clock phase

`cpc_reset()` left the CRTC divide-by-four accumulator and pre-render bus state
from the prior run. These sub-cycle fields are now reset with the machine.

## Deliberate or unresolved differences

### CPU/WAIT timing

`chips` advances one Z80 T-state at a time and lets the Gate Array assert WAIT
according to its sequencer phase. 1984 executes an instruction at a time, uses
Caprice32/konCePCja cycle tables with CPC wait costs included, and splits selected
I/O instructions around their bus access. This is much faster and already runs
timing-sensitive software, but it is not equivalent at arbitrary memory and I/O
edges. Converting to pin-level execution would be a separate CPU/bus project,
not a local Gate Array fix.

### FDC address aliases

`chips` selects the motor and uPD765 from only `A10`, `A8`, and `A7`, exposing
many aliases. 1984 and Caprice32 require high bytes `FA`/`FB` plus `A7=0`.
Broadening this decode could conflict with emulated expansion cards, so it was
not changed without a hardware test or a known program that requires an alias.

### RAM-configuration decode disagreement

`chips` treats RAM configuration as Gate Array function 3 and therefore also
requires the Gate Array address select. Caprice32 handles a `C0-C7` data byte on
any output with `A15=0`. 1984 currently agrees with `chips`. This needs a
hardware-backed test before changing it.

### Reset defaults

The `chips` CRTC reset preserves register contents and its Gate Array reset
clears all registers. 1984 reinitializes the CRTC to firmware-compatible timing
defaults and the Gate Array to a usable mode-1 palette. This differs during the
short interval before firmware initializes video, and on resets performed by
software that expects CRTC registers to survive. It is retained for now and
should be split into power-on versus reset behavior in a dedicated change.

### Scope where 1984 is broader

1984 supports selectable CRTC types 0-3, two floppy drives, cassette/CDT/WAV
paths, expansion ROMs and RAM, M4/MX4 peripherals, CPC Plus ASIC features, and
GX4000 cartridges. None of those extensions should be reduced to match the
simpler reference system.

## Verification

- Full `tests/Makefile` suite, including new PPI and I/O-decode tests.
- Clean `autoreconf -iv`, `./configure`, and `make -j4` in the SDL3 distrobox.
- CPC 6128 boot to BASIC and pasted `print 123` keyboard path.
- CP/M Plus boot from `cpmplus6128_1.dsk` to the `A>` prompt.
- CPC 6128 Plus boot to the system-cartridge menu.

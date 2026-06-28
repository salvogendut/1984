# LED activity bar

The thin strip below the CPC screen is a live activity bar. Each LED
maps to a different piece of hardware the emulator is currently
exposing — the slot only appears when the corresponding board is
enabled in the config or overlay. LEDs glow bright for ~120 ms after
each access and fade back to a dark idle colour.

## Slots

| Slot       | Idle / active colour     | Lit when                                                                 |
|------------|--------------------------|--------------------------------------------------------------------------|
| FDC A      | dark / bright red        | Floppy drive A read or write                                             |
| FDC B      | dark / bright red        | Floppy drive B read or write                                             |
| IDE        | dark / bright green      | SYMBiFACE II / Cyboard IDE access                                        |
| USB        | dark / bright blue       | Albireo CH376 USB host activity                                          |
| Net (KCNet)| dark / bright yellow     | Net4CPC W5100S register access                                           |
| USIfAC     | red ←→ green split       | RS232 RX (left half, red) and TX (right half, green) traffic             |
| Printer    | dark / warm amber        | Centronics parallel-port byte sent (`&EFxx`)                            |
| **M4**     | **3 segments, 1.5× wide**| **Power (blue, steady) + disk activity (red) + network activity (white)**|

## M4 LED — three segments

The M4 LED is wider than the others and split into three equal
coloured segments, left to right:

1. **Power** — blue, lit steadily whenever the M4 board is enabled.
   Confirms at a glance that the M4 firmware is mapped.
2. **Disk** — red, pulses on M4 SD-card / file-API commands
   (`C_SDREAD`, `C_SDWRITE`, file-search, mount, etc.).
3. **Net** — white, pulses on M4 networking commands
   (`C_NETSOCKET`, `C_NETCONNECT`, `C_NETCLOSE`, `C_NETSEND`,
   `C_NETRECV`, `C_NETHOSTIP`).

The split is the same idea as the USIfAC LED's RX/TX halves
(see [USIFAC.md](USIFAC.md)) extended to a third segment. Implemented
in `src/leds.{h,c}`; activity hooks live in `src/m4.c`.

## Enabling / disabling slots

LED visibility tracks the board toggles in `~/.config/1984/1984.conf`
(or the Advanced overlay):

| Slot   | Config key driving it                 |
|--------|---------------------------------------|
| FDC A/B| `model` (664/6128) or `dd1=true` (464)|
| IDE    | `rom_board=true` + `symbiface_ide=true`|
| USB    | `rom_board=true` + `albireo=true`     |
| Net    | `rom_board=true` + `net4cpc=true`     |
| USIfAC | `rom_board=true` + `usifac=true`      |
| Printer| `mx4=true` (Centronics port is always decoded on the bus)|
| M4     | `rom_board=true` + `m4=true`          |

Turning a board off hides its LED and re-centres the bar.

## Tuning

- `LED_GLOW_MS` (`src/leds.h`) — how long the bright state lingers
  after each ping. Default 120 ms.
- `LED_BAR_H` (`src/leds.h`) — bar height in pixels.
- Standard LED width is 24 px; the M4 LED renders at 1.5× (36 px)
  so the three segments stay legible.

# Cyboard

The **Cyboard** option on the **Extensions** tab (F9 overlay) is a single-shot
toggle that enables the full SYMBiFACE-compatible peripheral stack at once:

- **Net4CPC** — W5100S Ethernet
- **RTC** — DS12887 real-time clock
- **SYMBiFACE IDE** — FAT16/FAT32 disk image
- **SYMBiFACE Mouse** — PS/2 mouse via SDL relative capture

Enabling the Cyboard pack covers the *hardware* side. To actually use it from
BASIC and SymbOS you also need a matching ROM set in the expansion ROM slots,
because the firmware that talks to this hardware (UNIDOS + tools + file-system
nodes) lives in expansion ROMs — not in the stock OS/BASIC/AMSDOS bundle.

## Required ROMs

Open **F9 → Extensions → ROM Slots** and load the following:

| Slot | ROM             | Purpose                                                        |
|-----:|-----------------|----------------------------------------------------------------|
| 7    | `UNIDOS.ROM`    | DOS-node loader — must replace AMSDOS at slot 7               |
| 8    | `UNITOOLS.ROM`  | UNIDOS command-line tools (`|DIR`, `|CD`, `|TYPE`, etc.)      |
| 9    | `FATFS-P1.ROM`  | FAT file-system node, part 1                                  |
| 10   | `FATFS-P2.ROM` | FAT file-system node, part 2                                  |

Slots 8–10 are the conventional adjacent placements; UNIDOS scans for nodes in
slot order, so as long as the tools and FAT nodes sit just after `UNIDOS.ROM`
they will be picked up. If you have additional UNIDOS nodes (e.g. another
file-system or device), drop them in slots 11, 12, … in the same order.

> **Note:** Slot 7 is normally AMSDOS. Replacing it with UNIDOS is expected —
> UNIDOS provides the AMSDOS-equivalent disc-handling RSXes via its own command
> set. Disk A/B (`.dsk` images via the µPD765 FDC) keep working through the OS
> ROM regardless of slot 7's contents.

## Incompatibility with M4

The M4 board's `M4ROM.ROM` and UNIDOS both claim overlapping firmware territory
and cannot coexist. The overlay enforces this mutual exclusion:

- Enabling **Cyboard** disables **M4** (and triggers a cold boot).
- Enabling **M4** disables **Cyboard's RTC** and clears every expansion ROM
  override — including the slot-7 UNIDOS — restoring the stock BASIC and AMSDOS
  defaults.

So a switch from one stack to the other never requires editing the config file
by hand: pick the option you want and the other side is torn down for you.

> **Note:** This mutual exclusion is enforced only inside this emulator, to
> keep the configuration clean while M4 support remains experimental. It is
> not necessarily a real-hardware constraint — on actual CPCs the boards may
> well coexist if the physical ROM slots and I/O ports do not collide.

## Where to obtain the ROMs

| ROM             | Source                                               |
|-----------------|------------------------------------------------------|
| `UNIDOS.ROM`    | <https://unidos.cpcscene.net/>                       |
| `UNITOOLS.ROM`  | Bundled with the UNIDOS distribution                 |
| `FATFS-P1.ROM`  | Bundled with the UNIDOS distribution                 |
| `FATFS-P2.ROM` | Bundled with the UNIDOS distribution                 |

Download the UNIDOS distribution archive from the project site above; the
package contains `UNIDOS.ROM` plus the standard tool and node ROMs ready to
drop into the slots listed above.

## Verifying it works

After enabling Cyboard, loading the three ROMs, and booting:

1. At the BASIC prompt, type `|HELP` — UNIDOS and any loaded node ROMs should
   list their RSX commands.
2. Type `|DIR` — UNITOOLS should list files on the active FAT volume.
3. Switch volumes with `|CD,"path"` if you have multiple file systems mounted.

If `|HELP` shows only the standard AMSDOS commands, the UNIDOS ROM did not load
— double-check the slot 7 entry and confirm the file exists at the configured
path.

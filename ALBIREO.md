# Albireo

The **Albireo** entry on the **Extensions** tab (F9 overlay) emulates the
[Albireo USB host expansion](https://pulkomandy.github.io/shinra.github.io/albireo.html)
— a CH376-based USB controller. With UNIDOS loaded, BASIC sees USB mass-storage
volumes (Albireo's primary use); the same emulation also serves the raw USB
Bulk-Only Transport path that SymbOS uses for storage.

## Required ROMs

Open **F9 → Extensions → ROM Slots** and load the following:

| Slot | ROM           | Purpose                                                    |
|-----:|---------------|------------------------------------------------------------|
| 7    | `UNIDOS.ROM`  | DOS-node loader — must replace AMSDOS at slot 7            |
| 8    | `ALBIREO.ROM` | UNIDOS node for the Albireo USB host (CH376)               |

Slot 8 is the conventional adjacent placement; UNIDOS scans for nodes in slot
order, so as long as `ALBIREO.ROM` sits just after `UNIDOS.ROM` it will be
picked up.

> **Note:** Slot 7 is normally AMSDOS. Replacing it with UNIDOS is expected —
> UNIDOS provides the AMSDOS-equivalent disc-handling RSXes via its own command
> set. Disk A/B (`.dsk` images via the µPD765 FDC) keep working through the OS
> ROM regardless of slot 7's contents.

## Coexisting with Cyboard

Albireo and Cyboard are independent peripherals and can be enabled at the same
time. They both rely on UNIDOS at slot 7 (a single instance covers both), and
each one adds its own node ROM in a following slot.

A workable layout when both are on:

| Slot | ROM             |
|-----:|-----------------|
| 7    | `UNIDOS.ROM`    |
| 8    | `ALBIREO.ROM`   |
| 9    | `UNITOOLS.ROM`  |
| 10   | `FATFS-P1.ROM`  |
| 11   | `FATFS-P2.ROM` |

UNIDOS does not require any specific ordering past slot 7 — it scans forward
from there — so feel free to rearrange to suit your own setup. The key
constraint is that `UNIDOS.ROM` must be at slot 7 and the node/tool ROMs must
all sit at slots ≥ 8.

## Incompatibility with M4

The M4 board's `M4ROM.ROM` and UNIDOS both claim overlapping firmware territory
and cannot coexist. The overlay enforces this mutual exclusion:

- Enabling **Albireo** disables **M4** (and triggers a cold boot).
- Enabling **M4** clears every expansion ROM override — including the slot-7
  UNIDOS and any Albireo / Cyboard node ROMs — and disables both Albireo and
  Cyboard's RTC, restoring the stock BASIC and AMSDOS defaults.

## Where to obtain the ROMs

| ROM           | Source                                                |
|---------------|-------------------------------------------------------|
| `UNIDOS.ROM`  | <https://unidos.cpcscene.net/>                        |
| `ALBIREO.ROM` | Bundled with the UNIDOS distribution                  |

## Verifying it works

After enabling Albireo, loading the two ROMs, and booting:

1. Attach a USB image to the Albireo entry (overlay file picker) — a raw FAT
   image works.
2. At the BASIC prompt, type `|HELP` — UNIDOS and the Albireo node should list
   their RSX commands.
3. Type `|DIR,"usb:"` (or whatever volume name your node advertises) to list
   files on the attached image.

If `|HELP` shows only the standard AMSDOS commands, the UNIDOS ROM did not load
— double-check the slot 7 entry and confirm the file exists at the configured
path.

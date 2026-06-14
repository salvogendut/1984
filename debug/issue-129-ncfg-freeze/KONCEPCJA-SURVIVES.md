# #129 — konCePCja survives same scenario (2026-06-14)

konCePCja runs the same `realcyboard.img` with the same HDCPM.ROM,
SymbifaceII IDE, no Net4CPC and boots CP/M+ cleanly to the `A0:CPM>`
prompt — at the exact emulator-time window where 1984 cold-resets
(see `CRASH-PINPOINTED.md` for 1984's frame-3266 reset signature).

## konCePCja run

```bash
cd /var/home/salvogendut/Dev/konCePCja
./koncepcja --headless --cfg_file=/tmp/koncepcja-realcyboard.cfg \
    --autocmd="~PAUSE 1250~|hdcpm~ENTER~" \
    --exit-after=80s
# IPC at port 6543: take screenshot at +70s
```

Config: same as bundled, with `ide_master` re-pointed to
`/var/home/salvogendut/Downloads/realcyboard.img`. ROM slots identical
to 1984's setup (slot1=HDCPM, slot2-5=sym-romA-D, slot7=unidos,
slot8=unitools, slot9-10=fatfs).

## Result

`koncepcja-survives-70s.png` shows at +70s:

```
CP/M Plus  Amstrad Consumer Electronics plc
v 1.0, 59K TPA, 6 disc drives, 444K Dk'tronics ram drive M:
    ZCPR compatible system for CP/M+ by Simeon Cran

Code Page 437 set
Temporary Drive  - M:
Date format used - UK
Open NCFG.INI and search for HOME.
Net4CPC Interface not found !
       Show Help with -h
CP/M date and time synchronized with RTC.
16:56 A0:CPM>█
```

The "Net4CPC Interface not found" message is the *informational* response
NCFG prints when there's no Net4CPC hardware (exactly the configured
state). HDCPM completed disk scan, CP/M+ booted, ZCPR loaded, NCFG ran,
prompt is interactive.

## What this proves

The bug is in 1984's Z80 emulation, not in the disk image or in
HDCPM/CP/M+ itself. The cycle delta between konCePCja and 1984 at PC
region F0CD / CF3A / F229 is the fix surface — same shape as the work
in PR #128 (HDCPM boot race) and PR #111 (cc_op/cc_cb/cc_ed/cc_xy/cc_ex
table port from konCePCja).

## Next (task #62)

1. Use konCePCja IPC `wait pc 0xF0CD` to pause at the entry into the
   crashing code region.
2. Single-step a few hundred instructions logging each opcode + cycle
   count via `step trace`.
3. Run 1984 to the same point (use `--save-sna-at=3265` already
   archived, then re-load and trace forward).
4. Diff the two cycle streams — first divergence is the bug.

If konCePCja's IPC doesn't surface per-instruction cycle counts
directly, fall back to symbol-level: identify which CP/M+ routine
F0CD lives in (probably the BDOS device-scan code), build a
deterministic test harness around just that routine, and run it under
both emulators.

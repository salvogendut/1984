# #129 — crash pinpointed (2026-06-14)

Deterministic repro of the residual #129 cold-reset bug.

## Repro

```bash
cd /var/home/salvogendut/Dev/1984
pkill -9 -f "1984 --memory" || true; sleep 1
python3 - <<'EOF'
import os, time, subprocess, re
proc = subprocess.Popen(
    ["./1984", "--memory=576", "--ocr-monitor",
     "--config=debug/issue-129-ncfg-freeze/test-nonet.conf",
     "--screenshot-at=3280:/tmp/crash.ppm"],
    cwd="/var/home/salvogendut/Dev/1984",
    stderr=open("/tmp/crash.log","w"), stdout=subprocess.DEVNULL,
    env={**os.environ, "DISPLAY":":99", "SDL_VIDEODRIVER":"x11",
         "ONE_K_FAKE_RTC":"1", "ONE_K_FAKE_RTC_TIME":"12:00:00",
         "ONE_K_TRACE_IM1":"1"},
    preexec_fn=os.setsid)
time.sleep(2)
with open("/tmp/crash.log") as f:
    pty_path = re.search(r"kbd PTY: (\S+)", f.read()).group(1)
fd = os.open(pty_path, os.O_WRONLY | os.O_NONBLOCK)
time.sleep(30)
os.write(fd, b"|hdcpm\r")
proc.wait()
EOF
```

## Pinpointed moment

Bisection of `--screenshot-at=N` showed the cold-reset transition is between
frame 3225 (still "Scanning startup devices.") and frame 3275 (BASIC
banner). Narrowing further via `ONE_K_TRACE_IM1=1`:

| Frame | Event |
|------:|-------|
| 3260 | All 6 IRQs land at F1xx / F2xx / CFxx, SP=ABCB or close (normal CP/M+ run) |
| 3261-3265 | Same pattern — kernel stack stable around ABC3-ABCB |
| **3266** | 1st IRQ: PC=CF3A SP=ABC5 (normal); **2nd IRQ: PC=F0CD SP=BFD8** — SP just jumped to BASIC firmware stack |
| 3266-3267 | PCs swing between firmware (0Fxx/BAxx/BFxx) and CP/M+ area (Fxx) — chaotic |
| 3268 | PC=00CA SP=AB88 — already in CPC firmware reset path |
| 3270 | PC=C0E5 (executing 0xFF = RST 38 in unused ROM area) |
| 3273+ | Pure BASIC firmware loop, screen reset to BASIC banner |

**Diagnostic signature:** the SP transition `ABC5 → BFD8` between two
consecutive IRQ accepts inside frame 3266 — that's the cold-reset
moment. Reproducible with frozen RTC.

## Artifacts archived

- `sna-frame3265-prereset.sna` — Z80 snapshot from one frame BEFORE the
  reset event, useful for diffing against konCePCja running the same
  point.
- `screen-frame3225.png` — last "Scanning startup devices." frame.
- `screen-frame3260.ppm` — also still scanning; the SNA at 3265
  describes this state.
- `screen-frame3275.png` — first post-reset BASIC frame.

## Why this isn't what we thought

The morning report blamed host-TAP packet timing — proven wrong now:
this reproduces **with Net4CPC off**, no TAP whatsoever. The race is
inside the CP/M+ kernel ("Scanning startup devices."), well past
HDCPM ROM init. PR #128 stays intact (boot reaches CP/M+); the residual
race is in the BDOS device-scan code, not the BIOS load.

## Next steps (task #60)

1. Boot the same image in konCePCja with the same frozen RTC and watch
   what happens at the equivalent CP/M+ frame. If konCePCja survives,
   the cycle-table delta between konCePCja and 1984 at PC=F0CD-CF3A is
   the fix surface.
2. If konCePCja also crashes, the bug is in the disk image itself or
   in a hardware quirk we both share — escalate to disk content review.
3. Extract code bytes at PC=CF3A and PC=F0CD from the SNA, disassemble,
   look for an unmasked-EI path or a stack-corruption opportunity that
   only an IRQ delivered between specific Z80 cycles could trigger.

A useful tighter trace would be: dump SP for *every* IRQ accept (not
just PC), and stop emulation as soon as SP changes from the ~ABxx range
to the ~BFxx range. That would isolate the precise instruction-pair
where the stack got swapped.

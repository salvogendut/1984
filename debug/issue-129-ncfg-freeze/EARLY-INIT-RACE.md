# #129 — race localised to early-init host-side non-determinism (2026-06-14)

After the LDIR/iWSAdjust fix (commit 963486e), HDCPM/CP/M+ boot reaches
A0:CPM> in 7/10 manual runs and 2/3 headless probe runs. New finding:
**the runs that succeed are bit-identical to each other**, and the
runs that fail diverge from the success path very early in startup
— well before HDCPM gets to its disk-scan loop.

## Evidence

```bash
# 3 consecutive runs, ONE_K_FAKE_RTC=1, ONE_K_TRACE_IM1=1, Net4CPC=off
run 1: 26414 IRQs, screenshot PNG 3449 bytes → BASIC banner (FAIL)
run 2: 26236 IRQs, screenshot PNG 2741 bytes → 12:00 A0:CPM>   (PASS)
run 3: 26236 IRQs, screenshot PNG 2741 bytes → 12:00 A0:CPM>   (PASS)

diff -q <(grep '\[IM1\]' /tmp/dim-2.log) <(grep '\[IM1\]' /tmp/dim-3.log)
  # (no output — IM1 traces are byte-for-byte identical)
```

So with frozen RTC the boot is binary: takes the PASS path or the FAIL
path. Both paths are individually deterministic.

## Where the divergence happens

`compare_traces.py /tmp/dim-2.log /tmp/dim-1.log`:

```
First IRQ divergence at index 9562:
  PASS [9562] PC=0F1C SP=BFD0   ← inside firmware (lower ROM)
  FAIL [9562] PC=1BC6 SP=BFE2   ← inside BASIC ROM (upper ROM)
```

PASS path lands the CPU inside the firmware's `0x0F10` bit-shift loop
(`PUSH AF; PUSH HL; … RRC C; JR NC,-9`); FAIL path stays in BASIC
ROM's keyboard polling code. By IRQ #9562 the CPUs are executing
completely different code in completely different ROMs.

IRQ #9562 is ~9562 / 50 Hz ≈ 191 emulator-seconds in, but cycle math
puts the actual divergence INSTRUCTION earlier than that — somewhere
between IRQ #9561 (identical PC=1BCA SP=BFE2) and #9562, the CPU
executed ~8900 cycles where the two paths split. The split likely
happens much earlier in startup; by IRQ #9562 the consequences have
just become visible at IRQ boundary.

## What causes the early divergence?

Frozen RTC eliminates the wall-clock variation. Net4CPC=off eliminates
TAP packet timing. So the remaining non-determinism sources are
host-side, *before* the Z80 settles into its main loop:

- SDL audio thread initialisation timing
- Xvfb display setup variability
- Kernel scheduling jitter in the first few frames
- Open-file-handle ordering / fd numbers (affects PTY behaviour?)

The PASS path is whatever early-state config makes the LDIR fix's
cycle alignment land HDCPM cleanly. The FAIL path is the alignment
where it still hits the same residual race the LDIR fix only partially
covered.

## What this implies for fixes

Two complementary approaches:

1. **Close the race ENTIRELY by finishing the cycle alignment.**
   The LDIR fix moved 1984's cycle accounting closer to konCePCja's
   but doesn't cover every opcode konCePCja bumps differently. If we
   can find the remaining 1–2 cycle deltas, the FAIL path's alignment
   should converge with the PASS path. The bus-tick infrastructure
   from the reverted 5-commit stack would be helpful here, but only
   in combination with the right cc_op corrections.

2. **Eliminate the early-startup non-determinism.** Easier in
   principle: make 1984's first ~100 frames execute identically
   regardless of host scheduling. Concretely: defer SDL audio init,
   pre-warm the display before the Z80 begins running, freeze any
   wall-clock-dependent state besides the RTC. But this only HIDES
   the race — both alignments still exist, we just always pick one.

For now (2026-06-14): keep the LDIR fix committed on the branch but
DO NOT merge until either (1) closes more of the race or we
explicitly accept the 70% success rate as a meaningful interim
improvement. User pushback was clear: konCePCja-level precision is
the bar, not "better than before".

## Reproducer (3 runs in batch)

```python
# /tmp/dual-im1.py
import os, time, subprocess, re, signal
for i in [1, 2, 3]:
    proc = subprocess.Popen(
        ["./1984", "--memory=576", "--ocr-monitor",
         "--config=debug/issue-129-ncfg-freeze/test-nonet.conf",
         f"--screenshot-at=4500:/tmp/dim-{i}.ppm"],
        cwd="/var/home/salvogendut/Dev/1984",
        stderr=open(f"/tmp/dim-{i}.log","w"), stdout=subprocess.DEVNULL,
        env={**os.environ, "DISPLAY":":99", "SDL_VIDEODRIVER":"x11",
             "ONE_K_FAKE_RTC":"1", "ONE_K_FAKE_RTC_TIME":"12:00:00",
             "ONE_K_TRACE_IM1":"1"},
        preexec_fn=os.setsid)
    time.sleep(2)
    with open(f"/tmp/dim-{i}.log") as f:
        m = re.search(r"kbd PTY: (\S+)", f.read())
    fd = os.open(m.group(1), os.O_WRONLY | os.O_NONBLOCK)
    time.sleep(30)
    os.write(fd, b"|hdcpm\r")
    proc.wait()
```

Classify by screenshot PNG size: ~2741 bytes = A0:CPM> PASS,
~3449 bytes = BASIC FAIL.

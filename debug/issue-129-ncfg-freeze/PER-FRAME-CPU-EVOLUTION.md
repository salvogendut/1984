# #129 — per-frame CPU evolution across crash window

Captured with `ONE_K_DUMP_PC=1` and `--screenshot-at=N` for each N
listed below; `ONE_K_FAKE_RTC=1`, frozen at 12:00:00, Net4CPC OFF,
config = `test-nonet.conf`.

```
frame=3265 PC=CF3A SP=ABC5 AF=0040 BC=0002 DE=A84C HL=A928 IFF1=1 halted=0
frame=3266 PC=CF2A SP=ABC5 AF=0042 BC=0102 DE=A84C HL=CF34 IFF1=1 halted=0
frame=3267 PC=00CA SP=AB88 AF=2F00 BC=F581 DE=B63F HL=0000 IFF1=0 halted=0
frame=3268 PC=00CA SP=AB88 AF=2F00 BC=F581 DE=B63F HL=0000 IFF1=0 halted=0
frame=3270 PC=C582 SP=AB90 AF=0040 BC=0003 DE=AB96 HL=A849 IFF1=1 halted=0
```

## What this tells us

- Frames 3265–3266: clean CP/M+ kernel execution at PC=CF2A–CF3A,
  SP=ABC5, IFF1=1. The PC moves *backwards* by one byte (CF3A → CF2A)
  between frames, indicating a tight loop.
- Frame 3267: **catastrophic transition**. PC=00CA (inside firmware
  KL_DO_SYNC routine in lower ROM), SP=AB88, IFF1=0 (CPU is inside
  IRQ handler). All other regs changed.
- Frame 3268: identical to 3267 — CPU is stuck/idle at firmware IRQ
  handler entry.
- Frame 3270: CPU moved to PC=C582, SP=AB90 — different firmware
  routine, IFF1=1 (interrupts re-enabled).

## What 0x00CA is

Lower ROM bytes at 0x00C0 (OS_6128.ROM, `od` from offset 0xC0):

```
00C0  B9 B8 7C B7 C4 53 01 2A BB B8 7C B7 C4 53 01 CD
00D0  D7 20 21 BF B8 35 C0 36 06 CD F4 BD 2A BD B8 7C
```

Disassembly from 00C2 (00C0 is two data bytes):

```
00C2  LD A,H
00C3  OR A
00C4  CALL NZ, #0153          ; jump-to-background-ROM dispatch
00C7  LD HL,(#B8BB)
00CA  LD A,H                  ← PC AT FRAME 3267/3268
00CB  OR A
00CC  CALL NZ, #0153
00CF  CALL #20D7              ; (probably KL_DELIVER_SYNC)
00D2  LD HL,#B8BF             ; load tick counter
00D5  DEC (HL)
00D6  RET NZ                  ; return if not yet 6 ticks
00D7  LD (HL),6               ; reset counter
00D9  CALL #BDF4              ; KM_SCAN_KEYS
00DC  LD HL,(#B8BD)
00DF  LD A,H
```

This is the **CPC firmware's 50Hz tick / sync-dispatch routine** —
runs from the IM1 IRQ handler. Normally innocuous.

The bug: 1984's CPU got *stuck* here for two frames (3267–3268) with
IFF1=0, meaning the IRQ handler isn't returning. Combined with the IM1
trace showing SP at **0xBFD8** (BASIC firmware stack, not CP/M+ stack)
in the second IRQ of frame 3266, the picture is:

- CP/M+ kernel was running at CF3A (in upper-ROM-disabled RAM context).
- An IRQ fired and the CPU jumped to firmware IM1 handler at 0x0038.
- Somewhere during the handler's execution, the **upper ROM got
  re-enabled** and/or the **SP got reset to firmware values** (BFD8).
- The handler then went into a quasi-infinite loop or path that ended
  in a controlled-or-uncontrolled reset back to BASIC by frame 3275.

## Hypothesis: ROM-paging race during IM1 handoff

CP/M+ on the 6128 disables both upper and lower ROMs (via gate array)
so all of 0x0000-0xFFFF is RAM. The CP/M+ IM1 handler reads ROM bytes
via a *bank swap dance*: enables lower ROM, jumps to 0x0038, runs
firmware, returns to CP/M+ context.

If 1984's cycle counts for the **gate-array write** that re-disables
the ROM lag by 1–4 T-states vs konCePCja, an interrupt accepted
during that exact window can:
- Push the wrong PC (because the next-instruction fetch crossed a
  ROM/RAM boundary mid-flight)
- Land at firmware code with CP/M+ stack
- Eventually crash through firmware's reset path

This is the same shape as the bus-contention race PR #128 closed for
the HDCPM boot path, but firing one layer deeper inside the CP/M+
kernel.

## Next probe (task #62)

Cross-trace konCePCja vs 1984 at PC=CF3A and PC=00CA. Specifically:

1. Set `wait pc 0xCF3A` in konCePCja IPC, single-step a few hundred
   instructions logging cycle counts.
2. Reload `sna-frame3265-prereset.sna` in 1984 with a similar
   instruction-trace, comparing cycle counts.
3. The first opcode whose cycle count differs is the candidate.

If no simple cycle-table delta accounts for it, look at IM1 acceptance
timing in `cpu_step_with_irq` (1984's `src/z80.c`) vs konCePCja's
`runRefreshAndIrq()`. PR #128's fix was specifically in IRQ acceptance
math; this might need a sibling adjustment in CP/M+ context.

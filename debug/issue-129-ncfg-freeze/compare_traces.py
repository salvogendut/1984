#!/usr/bin/env python3
"""Pairwise trace comparison for PASS vs FAIL runs.

Each run produces a run<i>.log with interleaved IDE-CMD lines and
[IM1] lines. The PASS and FAIL runs are bit-identical up to some
specific moment, then diverge. This script:

  1. Reads two logs.
  2. Strips IM1 from one stream and IDE from another so we can find
     the first IDE LBA divergence.
  3. Finds the first IM1 line whose PC differs between the two.
  4. Prints a small window of context around each divergence point.

Usage:  compare_traces.py PASS_LOG FAIL_LOG
"""
import sys, re

def split(path):
    """Return two lists from a single log: (ide_cmds, im1_lines).

    Each IDE-CMD line is the LBA only (so we can diff easily). Each
    IM1 line keeps the PC + SP (frame number deliberately dropped
    because PASS and FAIL diverge in frame count over time)."""
    ide, im1 = [], []
    with open(path) as f:
        for line in f:
            m = re.match(r"\[IDE CMD #\d+\] cmd=.. LBA=(\S+)", line)
            if m: ide.append(m.group(1)); continue
            m = re.match(r"\[IM1\] frame=\d+\s+pushing PC=(\S+) SP=(\S+)", line)
            if m: im1.append((m.group(1), m.group(2)))
    return ide, im1

def first_diff(a, b):
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]: return i
    return n if len(a) != len(b) else -1

def context(seq, i, k=5):
    lo = max(0, i - k); hi = min(len(seq), i + k + 1)
    return [(j, seq[j]) for j in range(lo, hi)]

def main():
    if len(sys.argv) != 3:
        sys.exit("usage: compare_traces.py PASS.log FAIL.log")
    pass_log, fail_log = sys.argv[1], sys.argv[2]
    p_ide, p_im1 = split(pass_log)
    f_ide, f_im1 = split(fail_log)

    print(f"PASS: {len(p_ide)} IDE cmds, {len(p_im1)} IRQs")
    print(f"FAIL: {len(f_ide)} IDE cmds, {len(f_im1)} IRQs")
    print()

    ide_div = first_diff(p_ide, f_ide)
    print(f"First IDE divergence at index {ide_div}:")
    if ide_div >= 0:
        for j, lba in context(p_ide, ide_div):
            tag = " ← DIVERGE" if j == ide_div else ""
            print(f"  PASS [{j:4d}] LBA={lba}{tag}")
        print()
        for j, lba in context(f_ide, ide_div):
            tag = " ← DIVERGE" if j == ide_div else ""
            print(f"  FAIL [{j:4d}] LBA={lba}{tag}")
    print()

    im1_div = first_diff(p_im1, f_im1)
    print(f"First IRQ divergence at index {im1_div}:")
    if im1_div >= 0:
        for j, (pc, sp) in context(p_im1, im1_div):
            tag = " ← DIVERGE" if j == im1_div else ""
            print(f"  PASS [{j:5d}] PC={pc} SP={sp}{tag}")
        print()
        for j, (pc, sp) in context(f_im1, im1_div):
            tag = " ← DIVERGE" if j == im1_div else ""
            print(f"  FAIL [{j:5d}] PC={pc} SP={sp}{tag}")

if __name__ == "__main__":
    main()

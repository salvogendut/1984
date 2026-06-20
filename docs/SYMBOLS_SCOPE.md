# Symbol-import for the 1984 debugger — scope

Tracking issue: TBD. Feature requested by ajcasado (FUZIX-CPC maintainer)
to make FUZIX kernel/userspace debugging in 1984 tractable.

## Why

The F8 memory monitor today only speaks raw addresses. Disassembling
`CALL 0xC5EC` gives no clue whether that's `kern_main`, `tty_putc`, or
something in userspace at a totally different MMR banking. For a multi-bank
OS like FUZIX (where address `0x8000` can be either kernel CommonMem or
user-space code depending on MMR), the same address has *multiple* valid
symbol interpretations.

ajcasado specifically asks for:
> the ability to import symbols from the .map files produced during builds
> with both SDCC and SCC (the Fuzix Compiler Kit), ideally with the option
> to associate a RAM map with each .map file — specifically the MMR
> register value for the PAL / gate-array / RAM expansion setup.

## What we already have

- `src/z80dis.c` — pure-function disassembler, takes a memory pointer and a
  PC, returns a printable instruction. No symbol awareness today.
- `src/monitor.c` — F8 terminal-like UI with commands `D` (disasm), `M`
  (memory), `B`/`BC` (breakpoint set/clear), `N` (step), `G`/`GO`,
  `GA`/`CRTC` register dumps, `X`/`Q` (quit). Commands parsed at
  `mon_exec()` → `cmd_buf` strcmp ladder around line 284.
- `--monitor-pty` CLI flag — exposes the same prompt over a PTY for
  programmatic harnesses.
- `cpc->mem.ram_bank` carries the current MMR value at all times.

## Scope (MVP)

A new module `src/symbols.c` / `symbols.h` holding:

```c
typedef struct {
    u16 addr;
    char *name;
    char *module;       /* optional, may be NULL */
} Symbol;

typedef struct {
    char        *path;       /* original .map path, for messages */
    int          mmr_match;  /* -1 = match any MMR; else only when ram_bank == this */
    Symbol      *syms;
    size_t       n;          /* sorted by addr ascending */
} SymbolMap;

void symbols_load(const char *path, int mmr_match);   /* parses + registers */
const Symbol *symbols_lookup(u16 addr, u8 ram_bank);  /* best match for current bank */
const Symbol *symbols_lookup_name(const char *name);  /* case-sensitive exact */
void symbols_format(u16 addr, u8 ram_bank, char *out, size_t out_sz);
                     /* writes "name+0xNN" or "name" or empty if no match */
```

Lookup model: for the address `A` and current bank `B`, walk each loaded
`SymbolMap` whose `mmr_match` is `-1` or matches `B`, and pick the symbol
whose `addr` is the largest `<= A` (offset = `A - sym.addr`). Cap the
displayed offset (say 0x100 bytes); beyond that just print the raw address
to avoid pointing at the wrong function.

### CLI

```
--symbols=PATH                # load PATH, applies regardless of MMR
--symbols=MMR:PATH            # load PATH only when ram_bank matches MMR (hex byte)
```

May be repeated. Both SDCC and SCC formats detected by file signature (see
"Parser" below).

### Monitor commands

Add to `mon_exec()`:

| Command | Behaviour |
|---|---|
| `S name` | jump display + step source to `name`'s address |
| `S` | dump nearest symbol for current PC |
| `BS name` | set breakpoint at `name`'s address |

### Disasm enrichment

Patch the `D` command and the per-PC display line to call
`symbols_format(addr, ram_bank, …)` and suffix the output with
`  ; name+0xNN` when a match exists.

## Parsers

### SDCC `.map`

Lines we care about (real example from `~/Dev/FUZIX-codeberg/Kernel/fuzix.map`):

```
     0000ABCD  _kern_main                          mainmodule
```

Pattern: zero or more spaces, 8 hex digits, two-or-more spaces, identifier,
optional column with module name. Skip lines whose name starts with
`l__`, `s__`, `.` (segment markers) or begin with `--`/`A`/`Hexa`
(headers).

### SCC `.map` (Fuzix Compiler Kit)

Format spec not yet read. Two options:

1. Look at FuzixCompilerKit source on github.com/EtchedPixels/FuzixCompilerKit
   and produce a quick parser.
2. Ask ajcasado for a sample `.map` from an SCC build.

Plan to do (1) first; fall back to (2) if the format isn't self-documenting.

## Future increments (post-MVP)

- **Live `.lst` listing view** — if a per-function `.lst` is available next
  to the `.map`, show source/asm lines in the monitor at the current PC.
- **GhostNotation** — let the user write `;;` comments into a sidecar file
  that get displayed inline with the disassembly (for known-by-hand
  routines).
- **CALL/RET stack view** — keep a small ring of recent CALL sites with
  the symbol names, show as a stack trace in the monitor.
- **Per-bank ROM symbols** — load symbol files for AMSDOS, BASIC, the
  Albireo ROM, etc. and tag them with the appropriate MMR / upper-rom-
  select condition. Big win for boot-trace work too.

## What's intentionally out of scope

- DWARF or any other "real" debug info format. SDCC/SCC don't produce DWARF
  for Z80 targets and chasing it isn't worth the time.
- Source-line stepping. We can show *which function* the PC is in, but not
  which C line — that's much more work and lower value for FUZIX.
- Symbol editing in-monitor. Symbols are read-only after load.

## Files touched

- **New**: `src/symbols.c` / `src/symbols.h`
- **Modified**: `src/monitor.c` (new commands + disasm-line suffix),
  `src/main.c` (new CLI flag), `Makefile.am`, `docs/USAGE.md`.

## Verification

1. Load `~/Dev/FUZIX-codeberg/Kernel/fuzix.map`; in monitor, `S _kern_main`
   should jump to `kern_main`'s address. `D PC` after that should show
   `; _kern_main` next to the first line.
2. Step a few instructions; PC suffix updates with the offset.
3. With two maps loaded for different MMRs, the same address should produce
   different symbol names depending on `ram_bank`.
4. Behaviour with **no** `.map` loaded must be identical to today (no perf
   cost, no extra lines in the monitor).

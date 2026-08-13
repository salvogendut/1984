# SNA REMU Debug Metadata

1984 can consume and produce the `REMU` debugger chunk used by RASM
supersnapshots and ACE-compatible tools. The implementation is part of the
shared emulator core, so the same SNA works in the SDL3 application and the
WebAssembly application.

## Loading

The SNA v3 loader accepts both memory layouts in common use:

- a flat RAM dump after the 256-byte SNA header;
- `MEM0` through `MEMF` chunks, including the SNA `0xE5` RLE encoding used by
  RASM supersnapshots.

The following semicolon-terminated `REMU` records are understood by 1984:

| Record | Effect |
| --- | --- |
| `label NAME ADDRESS BANK` | Adds a physical 16 KB RAM-bank label. |
| `romlabel NAME ADDRESS BANK` | Adds a ROM-bank label. Bank 256 is the lower ROM. |
| `alias NAME VALUE` | Adds a named value for exact symbol lookup. |
| `brk ADDRESS BANK` | Imports a dormant RAM-bank-qualified execution breakpoint. |
| `rombrk ADDRESS BANK` | Imports a dormant ROM-bank-qualified execution breakpoint. |
| `acebreak EXEC,...,STOP,...` | Imports a dormant unconditional execution breakpoint when it has a full address mask, size 1, and no condition or stepping expression. |

Other records, including comments, memory/IO watches, conditional ACE
breakpoints, and future tags, are retained and written back when the snapshot
is saved. They are not acted on by the current debugger.

1984 has 16 shared execution-breakpoint channels. Snapshot breakpoints use
the same channels as breakpoints created in the native F8 monitor or the web
ML Monitor, but they start dormant. Debug metadata often describes the
assembler author's session and must not unexpectedly pause normal snapshot
playback. Loading another snapshot removes the prior snapshot's breakpoint
channels but retains breakpoints created by the user.

## Native SDL3

Load the SNA normally with `--load-sna=PATH`, the Media overlay, or the F8
monitor. Imported labels annotate disassembly. The monitor's `S NAME` and
`BS NAME` commands resolve REMU labels and aliases, while `B` lists imported
RAM/ROM bank qualifiers and their state. Use `BE N` to arm an imported
breakpoint and `BD N` to disarm it.

## WebAssembly

Use **Load SNA** in the web media deck. The ML Monitor uses imported labels
for disassembly while imported breakpoints remain dormant. Add an imported
address to the ML Monitor breakpoint list to arm it explicitly. Downloaded
snapshots retain the supported metadata and pass through unknown records.

## Saving

SNA v3 saves use 1984's flat RAM representation and append a `REMU` chunk when
there is debug metadata to write. The chunk contains:

- labels and aliases imported from a REMU chunk;
- imported and user-created non-temporary breakpoints;
- SDCC map symbols loaded with `--symbols`, converted to physical RAM-bank
  labels using their configured MMR mapping;
- unsupported input records preserved for round trips.

User-created unconditional breakpoints are emitted as ACE execution
breakpoints. This avoids incorrectly tying them to whichever RAM bank happened
to be visible when the snapshot was saved.

## Format References

- [RASM supersnapshot and REMU documentation](https://rasm.wikidot.com/dev%3Asupersnap)
- [cpclib REMU chunk implementation](https://github.com/cpcsdk/rust.cpclib/blob/master/cpclib-sna/src/chunks/remu.rs)

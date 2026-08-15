# RASM Supersnapshots and REMU Metadata

1984 can consume and produce the extended memory, ROM, and `REMU` debugger
chunks used by RASM supersnapshots and ACE-compatible tools. The implementation
is part of the shared emulator core, so the same SNA works in the SDL3
application and the WebAssembly application.

## Loading

The SNA v3 loader accepts these RAM layouts:

- a flat RAM dump after the 256-byte SNA header;
- `MEM0` through `MEMF` and `MX00` through `MX0F` 64 KB chunks, up to 1 MB
  (RASM uses `MX09` onward for bank sets above eight);
- raw 64 KB chunks or the SNA `0xE5` RLE encoding used by RASM.

RASM ROM chunks are also loaded:

| Chunk | Mapping |
| --- | --- |
| `LOWR` | 16 KB lower firmware ROM at `$0000-$3FFF`. |
| `RM00`-`RMFF` | 16 KB upper ROM selected at `$C000-$FFFF`. |

ROM payloads may be raw or RLE-compressed. They are held as sparse snapshot
overlays rather than replacing ROMs selected in `1984.conf`. Loading another
snapshot clears the overlays; a conventional SNA therefore reveals the user's
configured firmware and expansion ROMs again.

The following semicolon-terminated `REMU` records are understood by 1984:

| Record | Effect |
| --- | --- |
| `label NAME ADDRESS BANK` | Adds a physical 16 KB RAM-bank label. |
| `romlabel NAME ADDRESS BANK` | Adds a ROM-bank label. Bank 256 is the lower ROM. |
| `alias NAME VALUE` | Adds a named value for exact symbol lookup. |
| `comz ADDRESS BANK TEXT` | Adds a physical RAM-bank source comment. |
| `romcomz ADDRESS BANK TEXT` | Adds a ROM-bank source comment. Bank 256 is the lower ROM. |
| `brk ADDRESS BANK` | Imports a RAM-bank-qualified execution breakpoint. |
| `rombrk ADDRESS BANK` | Imports a ROM-bank-qualified execution breakpoint. |
| `acebreak EXEC,...,STOP,...` | Imports an unconditional execution breakpoint when it has a full address mask, size 1, and no condition or stepping expression. |

Labels and comments for the currently mapped physical bank annotate native and
browser disassembly. Other REMU records, including memory/IO watches,
conditional ACE breakpoints, and future tags, are retained and written back
when the snapshot is saved. They are not acted on by the current debugger.

RASM's binary `BRKS`, `BRKC`, and `SYMB` chunks are preserved byte-for-byte on
a save, but 1984 does not currently interpret them. Their contents may
therefore describe the original tool's debug session rather than edits made in
1984.

1984 stores execution breakpoints dynamically and assigns each one a stable
ID, with no fixed record-count limit. Native-monitor, DAP, snapshot, and
temporary stepping records retain separate ownership, so replacing DAP
breakpoints or loading another snapshot
does not remove breakpoints belonging to another source. Snapshot breakpoints
start armed by default to match RASM/ACE debug-session playback: the snapshot
resumes and stops when an embedded breakpoint is reached. **Snapshot
Breakpoints** can disable those records without affecting user or DAP records.

## Native SDL3

Load the SNA normally with `--load-sna=PATH`, the Media overlay, or the F8
monitor. Imported labels annotate disassembly. The monitor's `S NAME` and
`BS NAME` commands resolve REMU labels and aliases, while `B` lists imported
RAM/ROM bank qualifiers and their state. A recognized embedded breakpoint
automatically opens the monitor when hit. Use `BD ID` to disarm one breakpoint
or disable **Advanced -> Snapshot Breakpoints** to disarm all snapshot records.

## WebAssembly

Use **Load SNA** in the web media deck. The ML Monitor uses imported labels and
comments for disassembly. Recognized embedded execution breakpoints remain
snapshot-owned records, independently of breakpoints created through DAP. Its
**SNA Breaks** button globally arms or disarms snapshot records. The ML Monitor
opens and a toast reports when an armed breakpoint is hit.
Downloaded snapshots retain the supported metadata and preserved debug chunks.

## Saving

SNA v3 saves retain a conventional header and flat RAM image. This base remains
loadable by readers that ignore data after the RAM dump. 1984 then appends only
the supersnapshot extensions present in the session:

- `LOWR` and `RMxx` chunks imported from the loaded snapshot, using RASM RLE
  when it reduces their size;
- a `REMU` chunk when there is metadata to write;
- imported `BRKS`, `BRKC`, and `SYMB` chunks, unchanged.

Normal firmware and expansion ROMs loaded from 1984's configuration are not
embedded. The generated `REMU` chunk contains:

- labels, aliases, and source comments imported from a REMU chunk;
- imported and user-created non-temporary breakpoints;
- SDCC map symbols loaded with `--symbols`, converted to physical RAM-bank
  labels using their configured MMR mapping;
- unsupported input records preserved for round trips.

User-created unconditional breakpoints are emitted as ACE execution
breakpoints. This avoids incorrectly tying them to whichever RAM bank happened
to be visible when the snapshot was saved.

This design gives ordinary SNA readers a usable machine-state snapshot while
RASM-aware readers retain ROM and debug context. Exact compatibility still
depends on the other reader accepting optional chunks after a non-zero flat-RAM
size, as the v3 chunk convention permits.

## Format References

- [RASM supersnapshot and REMU documentation](https://rasm.wikidot.com/dev%3Asupersnap)
- [cpclib REMU chunk implementation](https://github.com/cpcsdk/rust.cpclib/blob/master/cpclib-sna/src/chunks/remu.rs)

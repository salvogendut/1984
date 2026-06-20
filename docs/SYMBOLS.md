# Symbol Maps

1984 can load SDCC asxxxx `.map` files and annotate the monitor with
the closest preceding symbol for each disassembled address. Useful when
debugging large Z80 programs (FUZIX kernel, CP/M binaries, custom
SDCC builds) where bare hex addresses don't tell you much.

## Quick start

```bash
# Apply a single map to every bank
1984 --symbols=/path/to/fuzix.map

# Apply only when the live RAM bank (Gate Array MMR) equals C2
# (the cpcsme FUZIX kernel sits here at runtime)
1984 --symbols=C2:/path/to/fuzix.map

# Repeatable — combine maps for different banks
1984 --symbols=C2:/path/to/kernel.map --symbols=C6:/path/to/loader.map
```

Then press **F8** to open the memory monitor and use the symbol-aware
commands below. The map is parsed once at startup; subsequent monitor
lookups are O(log n).

## Monitor commands

| Command | Effect |
|---|---|
| `S` | Show the symbol + offset for the current PC |
| `S <name>` | Look up `<name>` and start disassembling at its address |
| `BS <name>` | Set a breakpoint at the address of `<name>` |
| `D <addr>` | Standard disassembly — each line gets a `; symbol+offset` suffix when a map is loaded |

Press Enter after each command. The monitor works whether or not a
program has booted, but `D` / `S` are only useful once code is in memory.

## Supported map format

SDCC asxxxx textual `.map` (the default output of `sdas` / `sdld`):

```
00000000  s_HEADER0                          loader.asm
00000c3d  _bootdevice                        kernel.asm
0000c4ab  _create_init                       kernel.asm
```

The parser keeps the `name` and `addr` columns and filters out internal
markers (anything starting with `l__`, `s__`, or `.`).

## Per-MMR filtering

For paged operating systems (FUZIX, CP/M+) symbols only make sense
when the right bank is paged in. The `HEX:PATH` form gates lookups on
the live Gate Array MMR byte:

- `--symbols=C2:kernel.map` matches when `cpc->mem.ram_bank == 0xC2`
- `--symbols=PATH` (no prefix) matches every bank

The HEX value is the **raw MMR register value** (e.g. `0xC2`, `0xC6`),
not the bank number 0–7. If you don't know the bank, load with the
no-prefix form first, see where addresses fall, then tighten with
`HEX:`.

## Caveats

1. **The `.map` must match the binary actually running.** Loading a
   `.map` built from a different revision of the same program gives
   wrong addresses with no warning. Always pair a `.map` with the
   `.dsk` / `.bin` it was emitted alongside. This bit us once when we
   loaded a Codeberg-build FUZIX map and booted the upstream release
   `.dsk` — `BS _bootdevice` set a breakpoint at the wrong address and
   never fired.

2. **Strict equality on the MMR byte.** Per-MMR maps don't fuzzy-match
   bank ranges; only exact `==`. If your symbols span two banks, load
   the map twice with different prefixes, or use the no-prefix form.

3. **Only SDCC asxxxx maps are supported today.** SCC
   (FuzixCompilerKit) maps, `.lst` files, and ELF/DWARF debug info are
   out of scope on this branch.

4. **Disasm lines may truncate.** Symbol annotations are appended after
   the mnemonic. Long names plus long mnemonics can hit the monitor's
   right edge; bounded by `snprintf`, so no crash — just visual clip.

5. **Zero cost when unused.** Without `--symbols`, the disassembler
   pays one null-pointer compare per line and nothing else. No
   threads, no IO, no allocation at runtime.

## Example session

```bash
1984 --symbols=C2:/var/home/me/Dev/FUZIX-codeberg/Kernel/fuzix.map \
     --autostart=fuzix.dsk
```

In the monitor (`F8`):

```
> S
PC C3D0 — _bootdevice
> BS _create_init
breakpoint set at C4AB (_create_init)
> D C3D0
C3D0  21 50 C4    LD HL,C450h    ; _bootdevice
C3D3  CD 00 00    CALL 0000h     ; _bootdevice+0x3
C3D6  ...
```

## See also

- [USAGE.md](../USAGE.md) — full CLI reference and monitor command list
- [FUZIX_BUILD.md](FUZIX_BUILD.md) — building FUZIX from source, which
  produces the `fuzix.map` used as the primary test case

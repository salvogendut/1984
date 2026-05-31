# 1984 — Amstrad CPC Emulator

![1984](1984.png)

A cycle-stepped Amstrad CPC 464/6128 emulator written in C with SDL3.

## Status

Boots to Locomotive BASIC. Commercial games and standard software run well. Demos and other software that rely on undocumented hardware behaviour or cycle-exact CRTC tricks are untested and may not work correctly.

**M4 board emulation is unstable.** The ROM signature, FAT file API, single-image SAVE/LOAD/CAT, DNS resolution, and TCP connect plus the cpc-sdcc network examples (TCPTEST, NTP, TELNET) all work. SymbOS's `netd-m4c.exe` launches and resolves hosts but TCP sessions stall shortly after the initial server banner — multi-socket scenarios and long telnet sessions are not yet reliable.

## Features

**Core machine** — Z80 CPU (full documented set + undocumented IX/IY half-register ops), MC6845 CRTC, AY-3-8912 PSG (tone, noise, envelope), 8255 PPI, Gate Array with all 32 hardware colours, configurable RAM 64–1024 KB (DK'tronics + Yarek banking), 32 expansion ROM slots, F8 memory monitor + Z80 disassembler with breakpoints.

**Media** — DSK images via µPD765 FDC (drive A + B), AMSDOS file loading, `.cdt` / TZX cassette tape decoder with audio mixed into PSG output, AMSDOS-headed ROM auto-unwrap.

**Input** — keyboard, host-clipboard paste, USB/Bluetooth joystick and gamepad with hot-plug.

**Extensions** — DDI-1 floppy interface (464), DS12887 RTC (Cyboard / SYMBiFACE II), SYMBiFACE II / Cyboard IDE (FAT16/FAT32 disk images), SYMBiFACE II PS/2 mouse, Albireo USB mass-storage (CH376 host controller, driven by UNIDOS), Net4CPC Ethernet (W5100S, static IP — DHCP unsupported), Amstrad Diagnostics ROM as a one-click toggle.

## Supported platforms

| Platform | Status |
|---|---|
| Linux (x86_64) | Tested daily; Fedora RPM provided |
| Windows (MinGW-w64) | Tested under Wine and on native Windows 7+; CI-built `.exe` bundle |
| NetBSD | Builds and runs from pkgsrc |
| OpenBSD | Builds and runs |
| macOS | Builds and runs |
| Haiku (32-bit nightly) | Builds and runs |
| FreeBSD | Should work — autotools build is portable; not tested |

Pre-built Linux and Windows binaries are attached to each [GitHub Release](https://github.com/salvogendut/1984/releases).

## Screenshots

<table>
  <tr>
    <td align="center"><img src="screenshots/linux.png" width="280" alt="Linux"><br><sub>Linux</sub></td>
    <td align="center"><img src="screenshots/windows.png" width="280" alt="Windows"><br><sub>Windows</sub></td>
    <td align="center"><img src="screenshots/macos.png" width="280" alt="macOS"><br><sub>macOS</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="screenshots/haiku.png" width="280" alt="Haiku"><br><sub>Haiku</sub></td>
    <td align="center"><img src="screenshots/netbsd.png" width="280" alt="NetBSD"><br><sub>NetBSD</sub></td>
    <td align="center"><img src="screenshots/openbsd.png" width="280" alt="OpenBSD"><br><sub>OpenBSD</sub></td>
  </tr>
</table>

## Documentation

- **[INSTALL.md](INSTALL.md)** — installing pre-built binaries, building from source on each supported platform, ROM file requirements
- **[USAGE.md](USAGE.md)** — command-line options, keyboard shortcuts, joystick mapping, options overlay (F9), memory monitor (F8), config file format
- **[Development.md](Development.md)** — architecture, render pipeline, hardware emulation details, CI, port-specific notes (Haiku / NetBSD / Windows)

## Acknowledgements

- **[ColinPitrat/caprice32](https://github.com/ColinPitrat/caprice32)** — well-maintained CPC emulator; hardware colour palette RGB values used in `src/gate_array.c` were derived from Caprice32's colour table.
- **[CPCWIKI](https://www.cpcwiki.eu/)** — primary reference for CPC hardware documentation: CRTC registers and timing, Gate Array behaviour, memory map, I/O port decoding, and keyboard matrix.
- **[The Undocumented Z80 Documented](http://www.z80.info/zip/z80-documented.pdf)** (Sean Young) — reference for flag behaviour, undocumented opcodes (IX/IY bit instructions, DDCB/FDCB prefixes), and interrupt modes.
- **[µPD765 FDC Application Note](https://www.nec.com/)** and Amstrad CPC service manual — reference for the FDC command set and DSK image format used in `src/fdc.c` and `src/disk.c`.
- **[SDL3](https://github.com/libsdl-org/SDL)** — cross-platform library providing window management, rendering, audio, and input used throughout the emulator.
- **[llopis/amstrad-diagnostics](https://github.com/llopis/amstrad-diagnostics)** — Amstrad Diagnostics ROM used as an optional lower-ROM override for hardware testing (Diag Cart toggle in the options overlay).
- **[salafek/Net4CPC](https://github.com/salafek/Net4CPC)** — Net4CPC Ethernet add-on hardware design and W5100S interface reference; the emulated I/O ports (0xFD20–0xFD23) and register map in `src/net4cpc.c` follow this hardware specification.
- **[salafek/cyboard-for-cpc](https://github.com/salafek/cyboard-for-cpc)** — Cyboard hardware design; source of the DS12887 I/O port mapping (0xFD14/0xFD15) implemented in `src/rtc.c`.
- **[PulkoMandy's Albireo documentation](https://pulkomandy.github.io/shinra.github.io/albireo.html)** — CH376 / SC16C650B port map and command-flow notes used as the spec for `src/ch376.c`.
- **[UNIDOS](https://unidos.cpcscene.net/)** (OffseT/Futurs') — DOS-node ROM the Albireo emulation is verified against; `src/ch376.c` follows the CH376 command sequence in UNIDOS's `Albireo.rom` source (`LowLevel.a` / `DOSNode.a`).
- **[Prodatron's SymbOS](https://www.symbos.org/)** — multitasking operating system for Z80 machines; a key test target and reference for CPC system-level behaviour and expanded memory use.

## License

[GNU General Public License v2.0](LICENSE)

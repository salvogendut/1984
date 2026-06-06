# 1984 — Amstrad CPC Emulator

![1984](1984.png)

A cycle-stepped Amstrad CPC 464/6128 emulator written in C with SDL3.

## Status

Boots to Locomotive BASIC. Commercial games and standard software run well. Demos and other software that rely on undocumented hardware behaviour or cycle-exact CRTC tricks are untested and may not work correctly.

**HDCPM / CP/M+ boots from FAT-formatted SYMBiFACE II / Cyboard IDE drives**, mounting up to four `CPMDSKxx.IMG` images as CP/M drives A–D with the M: ramdisk, ZCPR shell, and SYMBiFACE RTC sync. **SymbOS desktop runs on both Albireo (USB / CH376) and Cyboard (Net4CPC + RTC + SYMBiFACE IDE).**

**M4 board emulation is partial.** The ROM signature, FAT file API, single-image SAVE/LOAD/CAT, DNS resolution, and TCP connect plus the cpc-sdcc network examples (TCPTEST, NTP, TELNET, WGET) all work. SymbOS's `netd-m4c.exe` launches, resolves hosts, and HTTP downloads via `wget` complete end-to-end. SymbOS interactive telnet sessions stall shortly after the initial server banner — the same stall is observed on real Net4CPC hardware, so this appears to be a SymbOS-side limitation rather than an emulator bug.

## Features

**Core machine** — Z80 CPU (full documented set + undocumented IX/IY half-register ops), MC6845 CRTC, AY-3-8912 PSG (tone, noise, envelope), 8255 PPI, Gate Array with all 32 hardware colours, configurable RAM 64–1024 KB (DK'tronics + Yarek banking), 32 expansion ROM slots, F8 memory monitor + Z80 disassembler with breakpoints.

**Media** — DSK images via µPD765 FDC (drive A + B), AMSDOS file loading, `.cdt` / TZX cassette tape decoder with audio mixed into PSG output, AMSDOS-headed ROM auto-unwrap.

**Input** — keyboard, host-clipboard paste, USB/Bluetooth joystick and gamepad with hot-plug.

**Extensions** — DDI-1 floppy interface (464), DS12887 RTC (Cyboard / SYMBiFACE II), SYMBiFACE II / Cyboard IDE (FAT16/FAT32 disk images), SYMBiFACE II PS/2 mouse, Albireo USB mass-storage (CH376 host controller, driven by UNIDOS), **Net4CPC Ethernet (W5100S) with one-click TAP backend on Linux — auto-creates the tap device, runs a built-in DHCP server, DNS proxy, and NAT to the host network. The CPC is fully on the LAN: pingable from the host, accepts inbound connections, and DHCP works end-to-end (verified in HDCPM + SymbOS)**, Amstrad Diagnostics ROM as a one-click toggle.

## Supported platforms

| Platform | Status |
|---|---|
| Linux (x86_64) | Tested daily; Fedora RPM provided |
| Windows (MinGW-w64) | Tested under Wine and on native Windows 7+; CI-built `.exe` bundle |
| NetBSD | Builds and runs from pkgsrc |
| OpenBSD | Builds and runs |
| macOS | Builds and runs |
| Haiku (32-bit nightly) | Builds and runs |
| FreeBSD | Builds and runs |

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
  <tr>
    <td align="center"><img src="screenshots/FreeBSD.png" width="280" alt="FreeBSD"><br><sub>FreeBSD</sub></td>
  </tr>
</table>

## Documentation

- **[INSTALL.md](INSTALL.md)** — installing pre-built binaries, building from source on each supported platform, ROM file requirements
- **[USAGE.md](USAGE.md)** — command-line options, keyboard shortcuts, joystick mapping, options overlay (F9), memory monitor (F8), config file format
- **[Development.md](Development.md)** — architecture, render pipeline, hardware emulation details, CI, port-specific notes (Haiku / NetBSD / Windows)

Per-expansion guides:

- **[M4.md](M4.md)** — M4 board (SD card + Wi-Fi networking); SymbOS netd-m4c.exe autostart caveat
- **[CYBOARD.md](CYBOARD.md)** — Cyboard pack (Net4CPC + RTC + SYMBiFACE IDE/Mouse); UNIDOS + UNITOOLS + FATFS ROM layout
- **[ALBIREO.md](ALBIREO.md)** — Albireo USB host (CH376); UNIDOS + ALBIREO.ROM layout; coexistence with Cyboard
- **[NET4CPC.md](NET4CPC.md)** — Net4CPC (W5100S) TAP backend; host-side device setup, bridge vs point-to-point, KCNet `NCFG.INI` profiles, `--trace-tap` walkthrough

## Acknowledgements

- **[ColinPitrat/caprice32](https://github.com/ColinPitrat/caprice32)** — well-maintained CPC emulator used extensively as a behavioural reference. Hardware colour palette RGB values in `src/gate_array.c` were derived from Caprice32's colour table; the µPD765 FDC semantics in `src/fdc.c` (status register layout per phase, ST0.AT/ST1.EN on EOT termination, settling delay on first EXEC MSR read, port 0xFB lo-byte bit-7 gating) and several Z80/Gate Array timing details (hsync falling-edge interrupt counter advance, CRTC tick cycle accumulator) follow Caprice32's implementation.
- **[ikari-pl/konCePCja](https://github.com/ikari-pl/konCePCja)** — Caprice32 fork by Cezar "ikari" Pokorski with a documented SYMBiFACE II implementation, IPC-controllable debugger and headless mode. Used as the gold-standard reference for the HDCPM / CP/M+ boot path: a byte-by-byte LBA-sequence comparison with konCePCja isolated the divergence point that led to the GA-interrupt acknowledge timing fix (deferring `ga_irq_ack()` until the Z80 actually accepts the maskable interrupt, plus restoring `IFF1` from `IFF2` on RETI — both behaviours mirroring konCePCja / Caprice32).
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

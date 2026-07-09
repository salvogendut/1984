# 1984 — Amstrad CPC Emulator

![1984](1984.png)

A cycle-stepped Amstrad CPC 464/664/6128 emulator written in C with SDL3.

## Status

Boots to Locomotive BASIC. Commercial games and standard software run well. Most demos and other software relying on undocumented hardware behaviour or cycle-exact CRTC tricks are untested but should, in general, run ok.

**HDCPM / CP/M+ boots from FAT-formatted SYMBiFACE II / Cyboard IDE drives**, mounting up to four `CPMDSKxx.IMG` images as CP/M drives A–D with the M: ramdisk, ZCPR shell, and SYMBiFACE RTC sync. **SymbOS desktop runs on both Albireo (USB / CH376) and Cyboard (Net4CPC + RTC + SYMBiFACE IDE).**

**M4 board emulation runs SymbOS unmodified.** ROM signature, FAT file API, single-image SAVE/LOAD/CAT, DNS resolution, TCP connect, send, and receive all work — both for cpc-sdcc network examples (TCPTEST, NTP, TELNET, WGET) and for SymbOS-native apps through `netd-m4c.exe`. The daemon may be left in SymbOS autostart; `settime.com` fetches the time over HTTP and the desktop clock updates end-to-end, `symtel` connects to telnet servers, `wget` downloads complete.

**FUZIX (ajcasado port, `cpcsme` platform) boots to a multi-user shell and networks over Net4CPC.** With a 6128 + 512 KB RAM + SymbIface IDE pointing at the FUZIX `disk.img`, the kernel boots through the splash, IDE probe, and partition table; entering `hda1` at the `bootdev:` prompt mounts the root filesystem and `root` logs in to the FUZIX shell. FUZIX also boots from an **Albireo (CH376) USB-mass-storage** image — point `albireo_image` at the same `disk.img` and the probe registers it as `hda` with `hda1`–`hda4` visible. **Net4CPC works end-to-end:** `ifconfig eth0 …` brings the interface up and `telnet <host>` reaches real Internet hosts (telehack banner verified). See [docs/issue-62-fuzix-notes.md](docs/issue-62-fuzix-notes.md).

<p align="center">
  <img src="screenshots/fuzix-boot.png" alt="FUZIX boot splash" width="380">
  &nbsp;&nbsp;
  <img src="screenshots/fuzix-shell.png" alt="FUZIX shell after login" width="380">
</p>

<p align="center">
  <img src="screenshots/fuzix-network.gif" alt="FUZIX networking works" width="480">
  &nbsp;
  <img src="screenshots/fuzix-network-2.gif" alt="FUZIX networking — second view" width="480"><br>
  <sub><b>FUZIX networking works</b></sub>
</p>

## Features

**Core machine** — cycle-stepped Z80 CPU (full documented set + undocumented IX/IY half-register ops, accurate interrupt/contention timing), MC6845 CRTC with overscan and hybrid screen modes (split-screen, mid-frame R-register reprogramming, rupture tricks), AY-3-8912 PSG (tone, noise, envelope with correct prescaler, sampled-audio playback), 8255 PPI, Gate Array with all 32 hardware colours, configurable RAM 64–1024 KB (DK'tronics + Yarek banking), 32 expansion ROM slots, `.sna` v1–v3 snapshot load/save, F8 memory monitor + Z80 disassembler with breakpoints.

**Media** — DSK images via µPD765 FDC (drive A + B), AMSDOS file loading, `.cdt` / TZX cassette tape decoder with audio mixed into PSG output, AMSDOS-headed ROM auto-unwrap.

**Display** — integer-scaled output 1×–4× (Ctrl ± at runtime) with smooth/sharp texture filtering, fullscreen with letterbox gutter. Optional **Real CRT** post-process with adjustable scanline opacity, brightness, contrast, and per-channel red/green/blue gain. **Monochrome** phosphor tint (green / amber / paper-white) emulating a period GT65-style monitor via a Rec. 601 luma transform. On-screen **toast notifications** for hardware events (USIfAC link-up, client connect/disconnect) — a fading bottom-left panel, toggleable between screen / console / off.

**Audio** — AY-3-8912 output as interleaved stereo with ABC channel panning (Caprice32-style, blendable from mono to full separation), a perceptual master volume control, and a DC blocker to kill register-toggle clicks. Tape loading screech mixed into the PSG output.

**Input** — keyboard, host-clipboard paste, USB/Bluetooth joystick and gamepad with hot-plug, scripted-joystick driver for automated UI tests.

**Host file exchange** — F10 pauses the guest and mounts every active card image (M4 SD / IDE / Albireo) on the host so files can be dragged in or out from the file manager. Uses `gnome-disk-image-mounter` / `udisksctl` for first-class Nautilus integration, with `guestmount` as a fallback. Press F10 again or eject the card from the file manager to unmount, sync, and cold-boot.

**Capture** — F4 still-screenshot (`.ppm`), F6 GIF screen-recording (in-tree LZW encoder, no dependencies), and WebM/VP9 recording via `ffmpeg` (optional, detected by `./configure`) — YouTube-native.

**Web GUI** — an embedded HTTP server (the Web GUI toggle under Advanced in the F9 overlay, or the `web_gui`/`web_port` config keys) that serves the **one running machine** to any browser on the network: live screen as a multipart GIF stream (in-tree encoder, no dependencies, ~25 fps with change detection so static screens cost nothing), browser-started audio as a streaming 44.1 kHz stereo WAV feed, full input capture — click the screen and the browser's keyboard **and mouse** (pointer lock, relative motion into the SYMBiFACE / Albireo / AMX pointer device, same dispatch as the emulator window) belong to the CPC until **Ctrl+Enter** releases them — plus a touch-friendly on-screen joystick, paste-text box, reset button, and per-drive "Load .dsk…" buttons that upload a disk image straight from the browser's file picker into drive A or B (stored under `~/.config/1984/web_uploads/`, mounted the same way the overlay's disk picker does). Single-threaded and polled from the frame loop like every other subsystem; up to 8 simultaneous connections, all watching/controlling the same machine; slow viewers just receive fewer frames. This is the "watch/control the machine I'm sitting at, from my phone too" mode — starting it keeps the host window visible.

**Web Service** (`--web[=PORT]`) — "emulator as a service": every distinct browser (strictly, every distinct cookie jar — multiple tabs in the same browser share one machine, a different browser or incognito profile gets its own) gets its **own, fully isolated CPC instance** on first visit, automatically, with the same video and browser-started audio streams as the Web GUI. Up to 4 concurrent sessions; a session with no attached viewer for 10 minutes is destroyed to free the slot, and a 5th concurrent session gets a friendly "all sessions busy" page instead of a machine. Sessions always boot from clean defaults — never the host user's real `~/.config/1984/1984.conf` — but a session can upload its own `.conf` via the page's "Load session config…" control to rewrite just that session's boot state (model, ROMs, disks, everything `1984.conf` covers). Each session gets the same disk-upload buttons as the Web GUI, scoped to that session's own `~/.config/1984/web_sessions/<token>/` directory (removed when the session is destroyed). `--web` implies `--headless`: no window on the host at all, the browser is the only interface, and it is a genuinely separate, self-contained server (`src/websvc.c`) — not just a headless flavor of the Web GUI above. For a permanent web appliance, a systemd **system** unit is installed as `1984-web.service`: `systemctl enable --now 1984-web` gives you a multi-user browser-only CPC service. It runs as a dedicated, shell-less `emulator` system account (created automatically, shared with 1985's equivalent unit if both are installed) rather than root or an interactive login user, with systemd sandboxing (`ProtectSystem=strict`, `NoNewPrivileges`, no capabilities, ...) on top — see the unit file's comments for the full rationale.

**Security note (both modes): they bind `0.0.0.0` with no authentication — anyone on your network can watch and type. Web Service's session isolation means a new browser gets a new *machine*, not new *credentials* — it is not a substitute for network trust. Enable either only on networks you trust.**

**Printer** — Centronics parallel port (`&EFxx`) wired to a Cairo-backed PDF surface on the host. `PRINT #8` / `LIST #8` / CP/M `LST:` / AMSDOS printer routing all land in timestamped PDFs in a user-picked directory. A "Real printer" mode spools each page to the host's default CUPS printer via `lp` (with `distrobox-host-exec` / `flatpak-spawn --host` escape so it works from sandboxed builds). Live in the overlay under Extensions, with the PDF/Real-printer sink selector under Advanced; cairo is optional (`--without-cairo` falls back to a no-op so the port is still decoded). LED bar gets a warm-amber slot for printer activity.

**Extensions** — DDI-1 floppy interface (464), DS12887 RTC (Cyboard / SYMBiFACE II), SYMBiFACE II / Cyboard IDE (FAT16/FAT32 disk images), SYMBiFACE II PS/2 mouse, Albireo USB mass-storage (CH376 host controller, driven by UNIDOS), **USIfAC II RS232 serial interface — wire-level emulation of the PIC-based board's serial pipe at `&FBD0`/`&FBD1`, with a host-side PTY or TCP listener as the backend, switchable live in the overlay. An optional stable symlink alias for the live `/dev/pts/N` slave lets external tools find the serial port at a fixed path across launches. A bundled **Retro Wi-Fi Modem** (PerryFi-inspired) rides on top of USIfAC: a Hayes-style AT modem that turns the serial port into a TCP dialler (`ATDT host:port`, `+++`, `ATH`, `ATO`) for CP/M comms tools and terminal programs. FUZIX completes its USIfAC handshake at boot; a split LED in the activity bar shows RX (red) and TX (green) traffic. See [docs/USIFAC.md](docs/USIFAC.md)**, **Net4CPC Ethernet (W5100S) with one-click TAP backend on Linux — auto-creates the tap device, runs a built-in DHCP server, DNS proxy, and NAT to the host network. The CPC is fully on the LAN: pingable from the host, accepts inbound connections, and DHCP works end-to-end (verified in HDCPM + SymbOS)**, Amstrad Diagnostics ROM as a one-click toggle. Per-board ROM groupings (`Ins` in the ROM Slots menu) tag each slot with the boards that need it — M4 / Albireo / Cyboard — so enabling a board auto-loads its ROMs and cached disk image; disabling clears them. No re-prompt for paths between sessions.

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
- **[USAGE.md](USAGE.md)** — command-line options, keyboard shortcuts, joystick mapping, options overlay (F9), memory monitor (F8), F10 host-side card browse, config file format
- **[Development.md](Development.md)** — architecture, render pipeline, hardware emulation details, CI, port-specific notes (Haiku / NetBSD / Windows)
- **[docs/FLATPAK.md](docs/FLATPAK.md)** — building and distributing the Flatpak from `main`; manifest layout, local bundle build, CI auto-build on tags

Per-expansion guides:

- **[M4.md](M4.md)** — M4 board (SD card + Wi-Fi networking); SymbOS netd-m4c.exe autostart caveat
- **[CYBOARD.md](CYBOARD.md)** — Cyboard pack (Net4CPC + RTC + SYMBiFACE IDE/Mouse); UNIDOS + UNITOOLS + FATFS ROM layout
- **[ALBIREO.md](ALBIREO.md)** — Albireo USB host (CH376); UNIDOS + ALBIREO.ROM layout; coexistence with Cyboard
- **[NET4CPC.md](NET4CPC.md)** — Net4CPC (W5100S) TAP backend; host-side device setup, bridge vs point-to-point, KCNet `NCFG.INI` profiles, `--trace-tap` walkthrough
- **[docs/USIFAC.md](docs/USIFAC.md)** — USIfAC II RS232 serial interface; PTY and TCP backend wiring, port map (`&FBD0/D1/D8/DD`), BASIC smoke tests
- **[docs/FUZIX_BUILD.md](docs/FUZIX_BUILD.md)** — building ajcasado/FUZIX from source against this emulator; cpctools/hex2bin/iDSK/flip shim, `CONFIG_USIFAC_SERIAL` mode toggle
- **[docs/SYMBOLS.md](docs/SYMBOLS.md)** — loading SDCC `.map` files for symbol-annotated disassembly in the F8 monitor; per-MMR matching, `S` / `BS` commands
- **[docs/LEDS.md](docs/LEDS.md)** — what each LED in the activity bar means; M4's 3-segment split (power / disk / net), USIfAC's RX/TX split, colour map per slot
- **[docs/PILOT.md](docs/PILOT.md)** — `--pilot` host-PTY auto-pilot; polar-coordinate command protocol for steering the mouse pointer or CPC joystick from a script or an AI during debugging

## Related projects

- **[1985](https://github.com/salvogendut/1985)** — sibling project: Amstrad PCW 8256 emulator built on the same SDL3 + autotools foundation as 1984.

## Acknowledgements

- **[ColinPitrat/caprice32](https://github.com/ColinPitrat/caprice32)** — well-maintained CPC emulator used extensively as a behavioural reference. Hardware colour palette RGB values in `src/gate_array.c` were derived from Caprice32's colour table; the µPD765 FDC semantics in `src/fdc.c` (status register layout per phase, ST0.AT/ST1.EN on EOT termination, settling delay on first EXEC MSR read, port 0xFB lo-byte bit-7 gating) and several Z80/Gate Array timing details (hsync falling-edge interrupt counter advance, CRTC tick cycle accumulator) follow Caprice32's implementation.
- **[ikari-pl/konCePCja](https://github.com/ikari-pl/konCePCja)** — Caprice32 fork by Cezar "ikari" Pokorski with a documented SYMBiFACE II implementation, IPC-controllable debugger and headless mode. Used as the gold-standard reference for the HDCPM / CP/M+ boot path: a byte-by-byte LBA-sequence comparison with konCePCja isolated the divergence point that led to the GA-interrupt acknowledge timing fix (deferring `ga_irq_ack()` until the Z80 actually accepts the maskable interrupt, plus restoring `IFF1` from `IFF2` on RETI — both behaviours mirroring konCePCja / Caprice32). The Z80 cycle tables in `src/z80.c` (`cc_op`, `cc_cb`, `cc_ed`, `cc_xy`, `cc_xycb`, `cc_ex`) are ported verbatim from konCePCja and fixed the residual HDCPM / CP/M+ kernel-ISR bank-save/restore race (#102) by getting CALL/RET/JR cycle counts into agreement with real Z80 timing.

  Between them, Caprice32 and konCePCja were indispensable references for cycle-exact CRTC and Z80 behaviour. Without studying their implementations and cross-checking 1984's output against them, supporting specific timing-sensitive demos and games (Bomb Jack, Batman Forever, and others that depend on undocumented hardware quirks or mid-frame CRTC tricks) would have been nearly impossible.
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

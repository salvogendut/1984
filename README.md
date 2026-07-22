# 1984 - Amstrad CPC emulator

![1984](1984.png)

1984 is a cycle-stepped Amstrad CPC 464, 664, and 6128 emulator written in C
using SDL3. It aims to run ordinary CPC software and timing-sensitive programs
while also modelling modern storage, networking, and input expansions.

## Current status

All three models boot their bundled firmware to Locomotive BASIC. Disk and
tape software, commercial games, and a growing set of timing-sensitive demos
run; Bomb Jack and Batman Forever are among the current CRTC/audio regression
targets.

| Workload | Current result |
|----------|----------------|
| Locomotive BASIC and AMSDOS | Working on CPC 464, 664, and 6128, with DDI-1 available for the 464 |
| HDCPM / CP/M Plus | Boots from SYMBiFACE II/Cyboard IDE images with CP/M drives, ramdisk, ZCPR, and RTC time |
| SymbOS with M4 | Boots with the unmodified M4 network daemon; time, Telnet, and HTTP applications work |
| SymbOS with Cyboard | Boots with Net4CPC, RTC, IDE storage, and SYMBiFACE mouse |
| SymbOS with Albireo | Desktop and storage paths boot, subject to the text-rendering/application-loading limitation below |
| FUZIX `cpcsme` | Boots from SYMBiFACE IDE or Albireo storage and reaches the Internet through Net4CPC |

See [docs/issue-62-fuzix-notes.md](docs/issue-62-fuzix-notes.md) and
[docs/FUZIX_BUILD.md](docs/FUZIX_BUILD.md) for the tested FUZIX setup.

<p align="center">
  <img src="screenshots/fuzix-boot.png" alt="FUZIX boot splash" width="380">
  &nbsp;&nbsp;
  <img src="screenshots/fuzix-shell.png" alt="FUZIX shell after login" width="380">
</p>

<p align="center">
  <img src="screenshots/fuzix-network.gif" alt="FUZIX networking through Net4CPC" width="600"><br>
  <sub><b>FUZIX using Net4CPC networking</b></sub>
</p>

## Core emulation

- CPC 464, 664, and 6128 model defaults, including the correct firmware,
  memory, floppy, and cassette configuration.
- Cycle-stepped Z80 with documented and commonly used undocumented opcodes,
  interrupt timing, and memory contention.
- MC6845 CRTC, Gate Array, 8255 PPI, overscan, split screens, hybrid display
  modes, mid-frame CRTC changes, and all 32 hardware colours.
- AY-3-8912 tone, noise, envelope, and sampled-audio playback.
- RAM configurations from 64 KB through 1 MB using DK'TRONICS and Yarek/RAM7
  banking.
- A 32-slot expansion ROM board, automatic removal of AMSDOS headers from ROM
  images, and board-tagged ROM/image profiles for M4, Albireo, and Cyboard.
- Amstrad SNA v1-v3 loading and v3 saving, plus an F8 monitor/disassembler
  with breakpoints and optional SDCC symbol maps.

The F9 overlay provides General, Media, Extensions, and Advanced tabs. MX4
controls the expansion bus. Roms Board controls generic user-managed ROM
slots, while board-tagged driver ROMs follow their active MX4 device. Enable
**Tinker** in General to expose the Advanced tab.

## Media and files

1984 reads standard and extended DSK images through the uPD765-compatible
floppy controller. Sector writes and FORMAT TRACK operations are persisted to
the mounted image. The Media tab can also create a blank 40-track CPC DATA
disk.

Drive A and B use the platform file picker by default. **Shift+Enter** opens
the built-in keyboard-driven DSK browser, and 1984 falls back to it
automatically when the platform picker is unavailable. `--sdl-fm` forces the
built-in browser, which makes disk changes usable on systems without a GUI
file manager.

CPC 664 and 6128 models have floppy hardware built in. On the 464, enabling
DDI-1 adds the controller and AMSDOS ROM. CDT/TZX cassette images support the
common standard, turbo, pure-tone, pulse, pure-data, and pause blocks; tape
audio is mixed into the AY output. The 464 deck is built in, while 664/6128
use the External Tape setting.

On Linux, **F10** can pause the guest and mount active FAT card images from M4,
SYMBiFACE IDE, and Albireo on the host. Pressing F10 again unmounts, syncs, and
cold-boots the CPC so guest filesystem caches cannot overwrite host changes.

## Display, audio, and input

The SDL display supports 1x through 4x integer scaling, smooth or sharp
filtering, and fullscreen letterboxing. Real CRT processing adds adjustable
scanlines, brightness, contrast, and RGB gain. Green, amber, and paper-white
monochrome modes derive luminance from the CPC colour image.

Audio is 44.1 kHz stereo with adjustable volume and ABC channel separation,
a DC blocker, and cassette sound. SDL gamepads and joysticks support hot-plug
and map to CPC joystick 1. The fallback input can instead be an AMX mouse;
SYMBiFACE PS/2 and Albireo CH376-A mouse devices are available with their
respective expansions. Click the emulator to capture a selected mouse and
press **Ctrl+Enter** to release it.

F4 saves a PPM screenshot. F6 records an animated GIF using the resolution,
frame-rate, and encoder profile under Advanced. The built-in path has no
dependencies; optional FFmpeg optimization reduces captures when possible.
Advanced > **Capture video** records WebM/VP9. Audio can also be recorded to
WAV from the command line.

## Expansion hardware

The MX4 expansion bus is enabled by default. Disabling it disconnects the
peripherals on the Extensions tab, including the printer; built-in model
hardware remains available.

| Expansion | Current implementation |
|-----------|------------------------|
| M4 | M4ROM, FAT image or host-directory file API, SD-sector access, clock, DNS, and TCP client operations |
| Net4CPC | W5100S register and socket model with four sockets; host-socket and TAP backends |
| Cyboard | Convenience control for Net4CPC, DS12887 RTC, SYMBiFACE IDE, and SYMBiFACE mouse |
| SYMBiFACE IDE | FAT16/FAT32 raw images with ATA identify, read, write, reset, and multi-sector transfers |
| Albireo | CH376 USB mass storage plus optional CH376-A HID mouse, usable from UNIDOS and FUZIX |
| USIfAC II | Wire-level serial pipe at `&FBD0`/`&FBD1`, backed by a PTY or localhost TCP listener on POSIX hosts |
| Wi-Fi Modem | PerryFi-inspired Hayes AT modem over USIfAC, with host TCP dial-out through `ATDT host:port` |
| Printer | Centronics output at `&EFxx`, captured to PDF with Cairo or spooled to `lp` |
| Diagnostics | Bundled Amstrad Diagnostics lower ROM selectable from the overlay |

M4 can now coexist with Net4CPC, RTC, SYMBiFACE IDE, and the SYMBiFACE mouse,
including the full Cyboard group. M4 and Albireo remain mutually exclusive
because both decode the `0xFExx` range and their firmware stacks conflict.
Board-tagged ROM profiles load and clear only the slots associated with the
board being toggled.

Net4CPC's default backend maps W5100S operations to host sockets. Its TAP
backend makes the CPC a layer-2 endpoint with ARP, ICMP, TCP, UDP, DHCP, and
DNS support. Linux auto-setup adds a private TAP network and NAT after one
privilege prompt. FreeBSD, NetBSD, and OpenBSD support TAP with manual host NAT
when wider network access is required. See [NET4CPC.md](NET4CPC.md).

The printer requires MX4 in this emulator. With Cairo enabled, output from
BASIC, AMSDOS, and CP/M is finalised into timestamped PDFs after an idle
period. Real Printer mode submits the PDF to the host's default CUPS printer.
Without Cairo the guest port still responds, but no host document is created.

## Web access

1984 has two browser-facing modes:

- **Web GUI** mirrors the one CPC already running in the SDL application. It
  streams screen and browser-started stereo audio, accepts keyboard, mouse,
  touch joystick, paste, and reset input, and can upload DSK images into either
  drive. Enable it from Advanced or with `web_gui=true`.
- **Web Service**, started with `--web[=PORT]`, runs headless and creates an
  isolated CPC for each browser cookie jar. It supports four concurrent
  sessions, session-specific configuration and disk uploads, and removes
  sessions ten minutes after their last viewer disconnects. Packages install
  the sandboxed `1984-web.service` system service.

Both modes bind to `0.0.0.0` and provide no authentication. Anyone who can
reach the port can view and control a machine, so use them only on a trusted
network. The default port is `1984`.

## Development and automation

The command line supports deterministic autostart and paste input, scripted
joystick motion, scheduled snapshot and screenshot capture, GIF and WAV
recording, headless execution, trace flags, and SDCC map symbols. PTY interfaces
are available for the monitor, keyboard/text output, OCR screen reader, and
`--pilot` mouse/joystick automation protocol. Run `./1984 --help` for the
current option list.

On-screen notifications report hardware and network events and can be routed
to the screen, console, or disabled. Hovering over an activity LED shows its
device label.

## Build and downloads

Fedora dependencies and build commands:

```bash
sudo dnf install gcc make autoconf automake pkgconf-pkg-config sdl3-devel cairo-devel
autoreconf -iv
./configure
make -j"$(nproc)"
./1984 --disk-a=/path/to/software.dsk
```

Cairo is optional (`./configure --without-cairo` disables PDF capture), and
`ffmpeg` is optional for WebM recording. The required CPC firmware, M4ROM, and
Amstrad Diagnostics ROMs are bundled in `roms/`.

Tagged releases publish a Linux x86_64 binary, Fedora RPM, Windows x86_64 zip,
and Flatpak bundle on the
[GitHub Releases page](https://github.com/salvogendut/1984/releases). Pushes to
`main` also produce workflow artifacts. Linux and Windows are built in CI;
source builds are maintained for macOS, FreeBSD, NetBSD, OpenBSD, and Haiku.
See [INSTALL.md](INSTALL.md) and [docs/FLATPAK.md](docs/FLATPAK.md).

## Keyboard shortcuts

| Key | Action |
|-----|--------|
| F4 | Save a PPM screenshot |
| F5 | Warm reset |
| F6 | Start or stop GIF recording |
| F8 | Open or close the monitor/disassembler |
| F9 | Open or close the options overlay |
| F10 | Mount or unmount active card images on the Linux host |
| F11 | Toggle fullscreen |
| F12 | Quit |
| Ctrl+= / Ctrl+- | Change window scale from 1x through 4x |
| Ctrl+V | Paste host clipboard text into the CPC |
| Click in window | Capture the active mouse device |
| Ctrl+Enter | Release captured mouse input |

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

## Configuration and guides

Settings are stored in `~/.config/1984/1984.conf` and can normally be managed
from F9. `--config=PATH` loads an alternate configuration without changing the
normal save destination.

- [USAGE.md](USAGE.md) - command line, keyboard, overlay, debugger, and config
- [Development.md](Development.md) - architecture, timing, and porting notes
- [M4.md](M4.md) - M4 storage and network setup
- [CYBOARD.md](CYBOARD.md) - Cyboard hardware and ROM layout
- [ALBIREO.md](ALBIREO.md) - Albireo/UNIDOS setup and current SymbOS caveat
- [NET4CPC.md](NET4CPC.md) - Net4CPC host-socket and TAP networking
- [docs/USIFAC.md](docs/USIFAC.md) - USIfAC PTY/TCP setup and port map
- [docs/SYMBOLS.md](docs/SYMBOLS.md) - SDCC symbols in the monitor
- [docs/LEDS.md](docs/LEDS.md) - activity LED layout
- [docs/PILOT.md](docs/PILOT.md) - PTY automation protocol

## Known limitations

- Timing-sensitive CPC software is broad; the named regression targets do not
  imply that every demo or undocumented CRTC technique has been verified.
- In SymbOS, Albireo raw-sector mode loads applications but corrupts some
  desktop text. Its file-command fallback renders text correctly but cannot
  launch applications. BASIC/UNIDOS and FUZIX storage are unaffected.
- M4 and Albireo cannot be enabled together in the current emulator.
- Net4CPC TAP is unavailable on Windows and macOS; those platforms use the
  host-socket backend. USIfAC/PerryFi host backends are also unavailable on
  Windows.

## Related project

[1985](https://github.com/salvogendut/1985) emulates the Amstrad PCW 8256,
8512, and 9512 using the same SDL3/autotools foundation and Z80 core.

## Acknowledgements

- [Caprice32](https://github.com/ColinPitrat/caprice32) and
  [konCePCja](https://github.com/ikari-pl/konCePCja) provided the main
  behavioural references for Z80 timing, CRTC/Gate Array behaviour, floppy
  semantics, and the HDCPM boot path.
- [CPCWiki](https://www.cpcwiki.eu/),
  [The Undocumented Z80 Documented](http://www.z80.info/zip/z80-documented.pdf),
  the NEC uPD765 documentation, and the Amstrad service manuals provided the
  hardware references.
- [SDL3](https://github.com/libsdl-org/SDL) provides the cross-platform video,
  audio, and input layer.
- [Amstrad Diagnostics](https://github.com/llopis/amstrad-diagnostics),
  [Net4CPC](https://github.com/salafek/Net4CPC),
  [Cyboard](https://github.com/salafek/cyboard-for-cpc),
  [Albireo documentation](https://pulkomandy.github.io/shinra.github.io/albireo.html),
  [UNIDOS](https://unidos.cpcscene.net/), and
  [SymbOS](https://www.symbos.org/) define or exercise the expansion hardware
  emulated here.

## License

[GNU General Public License v2.0](LICENSE)

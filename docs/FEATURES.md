# 1984 feature reference

This document contains the detailed capability and compatibility information
kept out of the project landing page. For operating instructions and command
line options, see [USAGE.md](../USAGE.md).

## Current status

All six machine profiles boot their bundled firmware or system cartridge.
Disk, tape, and CPR software, commercial games, and a growing set of
timing-sensitive demos run. Bomb Jack, Batman Forever, HBL, Sonic, and Eerie
Forest are among the current CRTC, ASIC, and audio regression targets.

| Workload | Current result |
|----------|----------------|
| Locomotive BASIC and AMSDOS | Working on CPC 464, 664, and 6128, with DDI-1 available for the 464 |
| CPC Plus cartridges | 464 Plus and 6128 Plus boot the bundled v4 system CPR; GX4000 boots its bundled Burning Rubber cartridge; all accept standalone RIFF CPR software |
| HDCPM / CP/M Plus | Boots from SYMBiFACE II/Cyboard IDE images with CP/M drives, ramdisk, ZCPR, and RTC time |
| SymbOS with M4 | Boots with the unmodified M4 network daemon; time, Telnet, and HTTP applications work |
| SymbOS with Cyboard | Boots with Net4CPC, RTC, IDE storage, and SYMBiFACE mouse |
| SymbOS with Albireo | Desktop and storage paths boot, subject to the limitation below |
| FUZIX `cpcsme` | Boots from SYMBiFACE IDE or Albireo storage and reaches the Internet through Net4CPC |

See [issue-62-fuzix-notes.md](issue-62-fuzix-notes.md) and
[FUZIX_BUILD.md](FUZIX_BUILD.md) for the tested FUZIX setup.

## Core emulation

- CPC 464, 664, 6128, 464 Plus, 6128 Plus, and GX4000 model defaults, with the
  appropriate firmware or cartridge, RAM, floppy, and cassette configuration.
- Cycle-stepped Z80 with documented and commonly used undocumented opcodes,
  interrupt timing, and memory contention.
- MC6845 CRTC, Gate Array, 8255 PPI, overscan, split screens, hybrid display
  modes, mid-frame CRTC changes, and all 32 hardware colors.
- AY-3-8912 tone, noise, envelope, and sampled-audio playback.
- Plus ASIC cartridge banking, register lock and mapping, 12-bit palette,
  hardware sprites, soft scroll, raster and split controls, and PSG DMA audio.
- RAM configurations from 64 KB through 1 MB using DK'TRONICS and Yarek/RAM7
  banking.
- A 32-slot expansion ROM board, automatic removal of AMSDOS headers from ROM
  images, and board-tagged ROM and image profiles.
- Amstrad SNA v1-v3 loading and v3 saving, plus an F8 monitor and disassembler
  with breakpoints and optional SDCC symbol maps.

The F9 overlay provides General, Media, Extensions, and Advanced tabs. MX4
controls the expansion bus. Roms Board controls generic user-managed ROM
slots, while board-tagged driver ROMs follow their active MX4 device. Enable
**Tinker** in General to expose experimental Advanced features.

## Media and files

1984 reads standard and extended DSK images through its uPD765-compatible
floppy controller. Sector writes and FORMAT TRACK operations are persisted to
the mounted image. The Media tab can create a blank 40-track CPC DATA disk.

Drive A and B use the platform file picker by default. **Shift+Enter** opens
the built-in keyboard-driven DSK browser, and 1984 falls back to it when the
platform picker is unavailable. `--sdl-fm` always uses the built-in browser.

With a mounted floppy highlighted, press **A** to browse its AMSDOS or CP/M
directory. Enter or Space marks a file, and **S** saves the session choice and
resets the CPC to run it. The choice survives machine resets and is cleared
when the disk is replaced, ejected, or the process exits. Before saving, press
**P** to remember the choice permanently for that disk image. Permanent
choices are stored as `[disk_autostart:N]` entries in `1984.conf`; disable P or
clear the mark and save to remove one.

The Plus models boot RIFF CPR cartridges. Select the model in General and the
cartridge in Media, or use `--464plus`, `--6128plus`, or `--gx4000` with
`--cartridge=PATH`. GX4000 is a fixed 64 KB console profile with no keyboard,
floppy drives, or cassette deck. Its media entries are disabled except for the
cartridge, and joystick or gamepad input remains available.

CPC 664 and 6128 profiles have floppy hardware built in. Enabling DDI-1 on the
464 adds the controller and AMSDOS ROM. CDT/TZX cassette support covers common
standard, turbo, pure-tone, pulse, pure-data, direct-recording, and pause
blocks. Tape audio is mixed into the AY output. The 464 deck is built in,
while the 664 and 6128 use the External Tape setting.

On Linux, **F10** pauses the guest and mounts active FAT card images from M4,
SYMBiFACE IDE, and Albireo on the host. Press F10 again to unmount, sync, and
cold-boot the CPC so guest filesystem caches cannot overwrite host changes.
DSK images use an AMSDOS filesystem and cannot be mounted through this path.

## Real cassette I/O

Real cassette I/O is an experimental Tinker feature under **Advanced > Real
Cassette**. On a CPC 664 or 6128, External Tape must also be enabled.

- **INPUT: System to CPC Deck** feeds the CPC from a host recording device or
  from a WAV file selected in Media.
- **OUTPUT: CPC Deck to System** routes a mounted CDT waveform or the CPC's
  cassette SAVE output to a host playback device or WAV file.
- **Capture to file** can record the selected output, copy system input, or
  convert a mounted WAV into a CDT direct-recording block (a "fat CDT").

Input gain, output level, device selection, a translucent waveform, an audible
monitor, and WAV remaining-time display are available where applicable.
Disabling Tinker closes all real-cassette host streams. See the Real Cassette
section in [USAGE.md](../USAGE.md) for the complete routing rules.

## Display, audio, and input

The SDL display supports 1x through 4x integer scaling, smooth or sharp
filtering, fullscreen letterboxing, and adjustable CRT scanlines, brightness,
contrast, and RGB gain. Green, amber, and paper-white monochrome modes derive
luminance from the CPC color image.

Audio is 44.1 kHz stereo with adjustable volume and ABC channel separation, a
DC blocker, and cassette sound. SDL gamepads and joysticks support hot-plug and
map to CPC joystick 1. Input can instead use an AMX mouse; SYMBiFACE PS/2 and
Albireo CH376-A mouse devices are available with their expansions.

F4 saves a PPM screenshot. F6 records an animated GIF using the Advanced
resolution, frame-rate, and encoder profile. Optional FFmpeg optimization can
reduce the result. Advanced > **Capture video** records WebM/VP9, and command
line automation can record audio to WAV.

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
| Wi-Fi Modem | PerryFi-inspired Hayes AT modem over USIfAC, with TCP dial-out through `ATDT host:port` |
| Printer | Centronics output at `&EFxx`, captured to PDF with Cairo or spooled to `lp` |
| Diagnostics | Bundled Amstrad Diagnostics lower ROM selectable from the overlay |

M4 can coexist with Net4CPC, RTC, SYMBiFACE IDE, and the SYMBiFACE mouse,
including the Cyboard group. M4 and Albireo remain mutually exclusive because
both decode the `0xFExx` range and their firmware stacks conflict.

Net4CPC's default backend maps W5100S operations to host sockets. Its TAP
backend makes the CPC a layer-2 endpoint with ARP, ICMP, TCP, UDP, DHCP, and
DNS. Linux can configure a private TAP network and NAT automatically after one
privilege prompt. The BSDs support TAP with manual host NAT. See
[NET4CPC.md](../NET4CPC.md).

The printer requires MX4. With Cairo enabled, BASIC, AMSDOS, and CP/M printer
output is finalized into timestamped PDFs after an idle period. Real Printer
mode submits the PDF to the host's default CUPS printer.

## Browser access

1984 has three browser-facing modes:

- **Javascript 1984** is the static WebAssembly edition. It can be used at
  <https://salvogendut.github.io/chimeric/js1984/> or self-hosted from
  `web/dist`. It provides CPC 6128 and 6128 Plus, local and server-hosted DSK
  and CPR media, SNA loading and downloading, Web Audio, keyboard, gamepad,
  AMX mouse, CRT controls, themes, and a DAP-based ML monitor. Guest media
  changes remain in browser memory.
- **Web GUI** mirrors the CPC running in the native SDL application. It streams
  video and browser-started stereo audio and accepts keyboard, mouse, touch
  joystick, paste, reset, and DSK uploads. Enable it under Advanced or with
  `web_gui=true`.
- **Web Service**, started with `--web[=PORT]`, runs headless and creates an
  isolated CPC for each browser cookie jar. It supports four concurrent
  sessions and expires idle sessions after ten minutes.

The native Web GUI and Web Service bind to `0.0.0.0` without authentication.
Use them only on a trusted network or behind an authenticated proxy. Their
default port is `1984`. The static WebAssembly edition follows the access
policy of the server publishing it. See [web/README.md](../web/README.md).

## Development and automation

The command line supports deterministic autostart and paste input, scripted
joystick motion, scheduled snapshot and screenshot capture, GIF and WAV
recording, headless execution, trace flags, and SDCC map symbols. PTY
interfaces are available for the monitor, keyboard and text output, OCR screen
reader, and `--pilot` mouse and joystick protocol. Run `./1984 --help` for the
current option list.

On-screen notifications report hardware and network events and can be routed
to the screen, console, or disabled. Hovering over an activity LED shows its
device label.

## Known limitations

- Timing-sensitive CPC software is broad; the named regression targets do not
  imply that every demo or undocumented CRTC technique has been verified.
- In SymbOS, Albireo raw-sector mode loads applications but corrupts some
  desktop text. Its file-command fallback renders text correctly but cannot
  launch applications. BASIC, UNIDOS, and FUZIX storage are unaffected.
- M4 and Albireo cannot be enabled together.
- Net4CPC TAP is unavailable on Windows and macOS; those platforms use the
  host-socket backend. USIfAC and PerryFi host backends are unavailable on
  Windows.

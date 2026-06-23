Name:           1984
Version:        0.4.9
Release:        1%{?dist}
Summary:        Amstrad CPC 464/6128 emulator

License:        GPL-2.0-only
URL:            https://github.com/salvogendut/1984
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  autoconf
BuildRequires:  automake
BuildRequires:  pkgconfig(sdl3)
BuildRequires:  desktop-file-utils
BuildRequires:  libappstream-glib

%description
1984 is a cycle-stepped Amstrad CPC 464/6128 emulator written in C
with SDL3. The core machine — Z80 CPU (including undocumented
IX/IY half-register opcodes), MC6845 CRTC, AY-3-8912 PSG audio,
µPD765 FDC, Gate Array, keyboard, joystick/gamepad, cassette
tape (.cdt/TZX images with loading screech mixed into PSG audio)
— runs commercial games and standard software at full speed.

Expansion peripherals emulated:
  * DS12887 real-time clock (Cyboard / SYMBiFACE II compatible)
  * SYMBiFACE II / Cyboard IDE (raw disk images, FAT16/FAT32)
  * SYMBiFACE II PS/2 mouse
  * Net4CPC W5100S Ethernet (TCP/UDP via host sockets, static IP)
  * Albireo USB host (CH376 driven by UNIDOS for file ops, raw
    USB Bulk-Only Transport for SymbOS storage)
  * M4 board (file API + ESP8266-style networking) — cpc-sdcc demos
    and SymbOS HTTP downloads (wget) work; SymbOS interactive telnet
    stalls after the server banner (same on real Net4CPC hardware)

DK'tronics-compatible RAM banking up to 576 KB and Yarek/RAM7
extended banking to 1024 KB are both supported. The F8 memory
monitor / disassembler with PTY interface, F9 options overlay
(model, RAM, MX4 expansion bus, Roms Board, drive/tape images,
all peripherals, ROM slots), F4 PPM screenshot with shutter
sound, and a paste-from-clipboard helper round out the package.

CPC firmware ROMs (OS_464, OS_6128, BASIC 1.0, BASIC 1.1,
AMSDOS), the open-source M4ROM, and the open-source Amstrad
Diagnostics ROM are bundled and installed under
%{_datadir}/%{name}/roms/.

%prep
%autosetup

%build
autoreconf -fiv
%configure
%make_build

%install
%make_install

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.salvogendut.Emulator1984.desktop
appstream-util validate-relax --nonet %{buildroot}%{_datadir}/metainfo/io.github.salvogendut.Emulator1984.metainfo.xml

%files
%license LICENSE
%doc README.md INSTALL.md USAGE.md
%{_bindir}/%{name}
%{_mandir}/man1/%{name}.1*
%{_datadir}/applications/io.github.salvogendut.Emulator1984.desktop
%{_datadir}/metainfo/io.github.salvogendut.Emulator1984.metainfo.xml
%{_datadir}/icons/hicolor/*/apps/io.github.salvogendut.Emulator1984.png
%dir %{_datadir}/%{name}
%dir %{_datadir}/%{name}/roms
%{_datadir}/%{name}/roms/AMSDOS.ROM
%{_datadir}/%{name}/roms/AMSDOS_664.ROM
%{_datadir}/%{name}/roms/AmstradDiagLower.rom
%{_datadir}/%{name}/roms/BASIC_1.0.ROM
%{_datadir}/%{name}/roms/BASIC_1.1.ROM
%{_datadir}/%{name}/roms/BASIC_664.ROM
%{_datadir}/%{name}/roms/M4ROM.ROM
%{_datadir}/%{name}/roms/OS_464.ROM
%{_datadir}/%{name}/roms/OS_664.ROM
%{_datadir}/%{name}/roms/OS_6128.ROM

%changelog
* Mon Jun 23 2026 Salvatore Bognanni <salvogendut@gmail.com> - 0.4.9-1
- Add Centronics parallel printer (port &EFxx) with Cairo PDF host
  sink and a "Real printer" mode that spools each page to the host's
  default CUPS printer via lp. Escapes distrobox / flatpak sandboxes
  via distrobox-host-exec and flatpak-spawn --host so the host's CUPS
  stack is reachable. New Extensions overlay entries (PDF printer +
  Printer mode), warm-amber LED, --printer-pdf=DIR / --printer-real
  CLI flags. Cairo is an optional dependency; --without-cairo falls
  back to a no-op host sink so the port is still decoded. Closes #162
  (#163).
- ppi: fix port B bit 6 (printer BUSY) polarity so the OS firmware
  printer routine actually proceeds to the OUT (&EFxx) write instead
  of waiting forever.

* Sun Jun 21 2026 Salvatore Bognanni <salvogendut@gmail.com> - 0.4.8-1
- Add 1984(1) man page (#159)
- Add Monochrome display tint: off / green / amber / white (#158)
- LED activity bar: 3-segment M4 LED (power / disk / net) (#154)
- Document LED slot map in docs/LEDS.md (#156)
- net4cpc: randomize ephemeral source port per-process (#153)
- symbols: import SDCC .map files into the F8 monitor (#152)
- m4: implement C_ROMLOW (0x433D) so FUZIX v2.0.7+ detects the board
  (#151)
- Fix FUZIX over Net4CPC: W5100S handshake, ephemeral port, Sn_IR
  clear, chip-wide IR summary (#150)
- Add scripted joystick + headless capture for automated UI tests
  (#149)
- usifac: add USIfAC II RS232 emulation (closes #36) (#148)
- Ship 664 ROMs (OS_664, BASIC_664, AMSDOS_664) in the package
- Distribute all in-tree headers and USAGE.md so `make dist` produces
  a buildable tarball

* Tue Jun 10 2026 Salvatore Bognanni <salvogendut@gmail.com> - 0.4.5-1
- Segfault fixes: file-dialog cancel state, NULL videocap path, dead
  FDC ternary, unguarded snapshot load (#120)
- Renderer perf: precomputed pen tables + cached resolved inks in
  render_char; per-pixel scalar work collapsed to a flat 16-pixel
  write (#121)
- Video capture: F6 GIF screen-recording (in-tree LZW encoder, no
  deps) plus Overlay → Capture video for WebM/VP9 via ffmpeg (#118)
- Snapshot load/save (.sna v1-v3), with --save-sna-at=N:PATH for
  headless capture (#100)
- Per-board ROM groupings — ROMs follow the M4 / Albireo / Cyboard
  toggle automatically (#103)
- Net4CPC TAP backend: one-click setup with built-in DHCP server,
  DNS proxy, and NAT; configurable subnet; CPC fully on the LAN
  (Linux); BSD if_tap backend for FreeBSD/NetBSD/OpenBSD (#109,
  #114)
- Stability: konCePCja cc_op cycle-table port, fixes the HDCPM /
  CP/M+ kernel-ISR bank-save/restore race (#102)
- Overlay: Debugging toggle in Advanced + bottom-left FPS counter
- Overlay: ROM-slot Del also clears the board tag + template (#117)

* Sun May 31 2026 Salvatore Bognanni <salvogendut@gmail.com> - 0.4.3-1
- Stabilise SymbOS networking on M4 — sock_info window extended to 16
  slots with active TCP socket broadcast across all slots; wget HTTP
  downloads via SymbOS netd-m4c.exe now complete end-to-end (interactive
  telnet still stalls after the server banner; same behaviour reproduces
  on real Net4CPC hardware)
- Overlay: enforce M4 ↔ Cyboard / M4 ↔ Albireo mutual exclusion in both
  directions; enabling M4 tears down the full Cyboard pack (Net4CPC,
  RTC, IDE, Mouse) and clears every expansion ROM override; enabling
  Cyboard or Albireo disables M4. All transitions trigger a cold boot.
- Config: sanitise legacy configs on load — when m4=true together with
  rtc/albireo=true, keep M4 and clear the conflicting peripherals.
- Overlay label "M4 (unstable)" → "M4 (experimental)".
- New per-expansion docs: M4.md, CYBOARD.md, ALBIREO.md (ROM-slot
  layouts, cross-stack notes, SymbOS netd-m4c.exe autostart caveat).
- Add macOS and OpenBSD to the supported-platforms table with
  screenshots; INSTALL.md split accordingly.

* Sun May 31 2026 Salvatore Bognanni <salvogendut@gmail.com> - 0.4.2-1
- Port to Windows (MSYS2 / MinGW-w64) — full build, runs under Wine
  and on native Windows; .exe + SDL3.dll + ROMs published as a CI
  artifact on every push
- Port to Haiku (32-bit nightly tested) — configure.ac auto-detects
  the secondary-arch pkg-config path and links libnetwork for sockets
- Document NetBSD build (pkgsrc deps, ACLOCAL_PATH, gmake) — no
  source changes required
- Add GitHub Actions build matrix (Fedora + MinGW) with package
  caching; tagged pushes now auto-publish a GitHub Release with both
  platforms' binaries attached
- Fix "could not create config file" on fresh accounts where
  ~/.config/ does not yet exist (mkdir the parent first)
- Fix EXCEPTION_STACK_OVERFLOW at startup on Windows (move 1 MB CPC
  struct from stack to BSS)

* Sat May 30 2026 Salvatore Bognanni <salvogendut@gmail.com> - 0.4.1-1
- Add cassette tape support — .cdt (TZX) decoder, block types
  0x10/11/12/13/14/20, audio mixed into PSG output (loading screech
  on real hardware)
- Add Media -> Tape file picker; tape is always wired on CPC 464,
  CPC 6128 needs the new General -> External Tape toggle (visible
  only when the 6128 model is selected)
- Fix letterbox gutter in fullscreen sometimes drawn in overlay
  colours (yellow / blue leaked when the renderer's draw colour was
  left set by an overlay text or rect)

* Sat May 30 2026 Salvatore Bognanni <salvogendut@gmail.com> - 0.4.0-1
- Add DS12887 real-time clock (Cyboard / SYMBiFACE II compatible)
- Add SYMBiFACE II / Cyboard IDE (FAT16/FAT32 raw disk images)
- Add SYMBiFACE II PS/2 mouse with SDL relative capture
- Add Net4CPC W5100S Ethernet (TCP/UDP via host sockets, static IP;
  DHCP not supported — requires TUN/TAP)
- Add Albireo USB host emulation (CH376): full UNIDOS file-system
  command set, raw USB Bulk-Only Transport for SymbOS storage,
  USB HID mouse path
- Add M4 board emulation: file API over FAT, BASIC SAVE/LOAD/CAT,
  ESP8266-style TCP networking (UNSTABLE for SymbOS netd-m4c.exe;
  cpc-sdcc TCPTEST/NTP/TELNET examples work)
- Add memory monitor / disassembler (F8) with PTY interface
- Overlay overhaul: General / Media / Extensions tabs;
  new MX4 toggle gates the entire expansion bus;
  new Roms Board toggle gates the 32-slot expansion ROM card;
  OS ROM / BASIC ROM rows are now editable via file picker;
  Media tab adds a Tape entry (.cdt picker, stub for now)
- Add Yarek/RAM7 banking — RAM expansion to 1024 KB
- Add F4 PPM screenshot with shutter sound; drop libpng dependency
- Add trace flags: --trace-input, --trace-m4, --trace-albireo,
  --trace-net4cpc, --trace-io, --trace-palette
- Add ROM bundle — AMSDOS, BASIC 1.0/1.1, OS 464/6128, M4ROM,
  AmstradDiagLower installed to %%{_datadir}/%%{name}/roms/
- Add ROM auto-strip for 128-byte AMSDOS-headed files (16512 B)
- Fix Spindizzy palette regression on 464 and 6128 via fallback
  palette-flush gating
- Fix SymbOS joystick/keyboard input and screen-stripes regression

* Tue May 26 2026 Salvatore Bognanni <salvogendut@gmail.com> - 0.3.0-1
- Add expansion ROM slots 0-31 configurable via options overlay
- Add DDI-1 floppy interface option for CPC 464
- Fix ROM paths always using full ~/.config/1984/roms/ directory
- Fix first-run ROM path bug (was using relative paths)

* Tue May 26 2026 Salvatore Bognanni <salvogendut@gmail.com> - 0.2.0-1
- Add joystick/gamepad support (USB, Bluetooth, hot-plug)
- Add AY-3-8912 PSG audio
- Add expansion ROM slots (0-31) configurable via options overlay
- Add DDI-1 floppy interface option for CPC 464
- Add autoconf/automake build system
- Add -h/--help and unrecognised option handling
- Add GPLv2 license

* Tue May 26 2026 Salvatore Bognanni <salvogendut@gmail.com> - 0.1.0-1
- Initial package

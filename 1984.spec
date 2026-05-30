Name:           1984
Version:        0.4.1
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
  * M4 board (file API + ESP8266-style networking) — currently
    unstable for the SymbOS netd-m4c.exe daemon, otherwise usable

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
%doc README.md
%{_bindir}/%{name}
%{_datadir}/applications/io.github.salvogendut.Emulator1984.desktop
%{_datadir}/metainfo/io.github.salvogendut.Emulator1984.metainfo.xml
%{_datadir}/icons/hicolor/*/apps/io.github.salvogendut.Emulator1984.png
%dir %{_datadir}/%{name}
%dir %{_datadir}/%{name}/roms
%{_datadir}/%{name}/roms/AMSDOS.ROM
%{_datadir}/%{name}/roms/AmstradDiagLower.rom
%{_datadir}/%{name}/roms/BASIC_1.0.ROM
%{_datadir}/%{name}/roms/BASIC_1.1.ROM
%{_datadir}/%{name}/roms/M4ROM.ROM
%{_datadir}/%{name}/roms/OS_464.ROM
%{_datadir}/%{name}/roms/OS_6128.ROM

%changelog
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

Name:           1984
Version:        0.3.0
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
BuildRequires:  pkgconfig(libpng)

%description
1984 is a cycle-stepped Amstrad CPC 464/6128 emulator written in C
with SDL3. It emulates the Z80 CPU, MC6845 CRTC, AY-3-8912 PSG
(audio), µPD765 FDC (disk), and the Gate Array. Keyboard, joystick,
and gamepad input are supported.

CPC firmware ROMs (OS/BASIC/AMSDOS), the open-source M4ROM, and the
Amstrad diagnostics ROM are bundled and installed under
%{_datadir}/%{name}/roms/.

%prep
%autosetup

%build
autoreconf -fiv
%configure
%make_build

%install
%make_install

%files
%license LICENSE
%doc README.md
%{_bindir}/%{name}
%{_datadir}/%{name}/roms/

%changelog
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

Name:           1984
Version:        0.1.0
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

ROM images are not included and must be supplied separately.

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

%changelog
* Mon May 26 2026 Salvatore Bognanni <salvogendut@gmail.com> - 0.1.0-1
- Initial package

# Installing 1984

Two paths: download a pre-built binary, or build from source.

## Pre-built binaries

Every release on the [GitHub Releases page](https://github.com/salvogendut/1984/releases) attaches binaries for the platforms below. The same files are also produced as workflow artifacts on every push to `main` ([Actions tab](https://github.com/salvogendut/1984/actions)) if you want the latest unreleased build.

| Asset | Platform | Contents |
|---|---|---|
| `1984-vX.Y.Z-linux-x86_64` | Linux x86_64 | Standalone ELF; needs SDL3 ≥ 3.0 installed system-wide |
| `1984-vX.Y.Z-windows-x86_64.zip` | Windows x86_64 | `1984.exe` + `SDL3.dll` + MinGW runtime DLLs + bundled ROMs; unzip and run |

The Windows zip is fully self-contained — extract anywhere and double-click `1984.exe`. No system installation needed.

## Build from source

Requirements:

- GCC supporting C11 (or any C11-capable compiler)
- SDL3 (development headers)
- `autoconf`, `automake`, `pkg-config`, GNU `make`

Optional runtime tools (auto-detected by `./configure`):

- `ffmpeg` — enables the WebM/VP9 video-capture path in the overlay. Without it, F6 still records GIFs (no dependencies).

### Linux

```bash
autoreconf -iv
./configure
make
sudo make install              # optional; otherwise run ./1984 in-place
```

Fedora / RHEL / CentOS users can build an RPM directly from the spec file:

```bash
rpmbuild -ba 1984.spec
```

A simpler hand-written `Makefile` is also provided for quick iteration; it produces `bin/1984` and skips the autoconf dance entirely:

```bash
make
./bin/1984
```

### Windows (MSYS2 / MinGW-w64)

From an MSYS2 MinGW64 shell:

```bash
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-pkgconf \
                   mingw-w64-x86_64-sdl3 autoconf automake make
autoreconf -iv
./configure
make
```

`configure` detects `host_os = mingw*` and links `-lws2_32` for sockets plus `-mconsole` so stderr/stdout reach `cmd.exe`. `1984.exe` needs `SDL3.dll` and the MinGW runtime DLLs from `/mingw64/bin/` alongside it to run.

### Haiku

```bash
pkgman install libsdl3_x86_devel pkgconfig autoconf automake
setarch x86          # switch this shell to the modern GCC (32-bit Haiku)
autoreconf -iv
./configure
make
```

`configure` auto-detects Haiku and finds SDL3's secondary-arch `.pc` file without any manual `PKG_CONFIG_PATH`. On 64-bit Haiku use `setarch x86_64` and the corresponding non-`_x86` package names.

### NetBSD (pkgsrc)

```sh
sudo pkgin install pkgconf autoconf automake gmake SDL3
export ACLOCAL_PATH=/usr/pkg/share/aclocal
autoreconf -iv
./configure
gmake
```

`ACLOCAL_PATH` is required so `aclocal` finds pkg-config's `pkg.m4` (pkgsrc installs it under `/usr/pkg/share/aclocal/`, which is not on the default search path). Build with `gmake`, not BSD `make` — the automake-generated Makefile uses GNU-make-isms.

### macOS / OpenBSD

Builds and runs with the standard `autoreconf -iv && ./configure && make`. Install SDL3 + autotools via Homebrew (macOS) or `pkg_add` (OpenBSD).

### FreeBSD

Should work — the autotools build is portable. Not tested; please open an issue if it doesn't.

## ROM files

The required CPC firmware ROMs and the open-source M4ROM / Amstrad Diagnostics ROM are bundled in the `roms/` directory and installed to `$(datadir)/1984/roms/` by `make install` (e.g. `/usr/share/1984/roms/`).

| File | Contents |
|------|----------|
| `OS_464.ROM` | CPC 464 OS ROM (16 KB) |
| `BASIC_1.0.ROM` | CPC 464 Locomotive BASIC 1.0 (16 KB) |
| `OS_664.ROM` | CPC 664 OS ROM v2 (16 KB) |
| `BASIC_664.ROM` | CPC 664 Locomotive BASIC v1.1.0 (16 KB) — note: NOT the same as `BASIC_1.1.ROM`, which is actually v1.2.0 (the 6128's BASIC) |
| `AMSDOS_664.ROM` | CPC 664 AMSDOS (16 KB) — built-in on the 664 |
| `OS_6128.ROM` | CPC 6128 OS ROM (16 KB) |
| `BASIC_1.1.ROM` | CPC 6128 Locomotive BASIC v1.2.0 (16 KB) — historical naming; actual ROM version is 1.2 |
| `AMSDOS.ROM` | AMSDOS disk filing system (16 KB) — required for disk access on 6128; optional on 464 (needs DD1) |
| `M4ROM.ROM` | M4 board firmware (16 KB) — open source ([M4Duke/m4rom](https://github.com/M4Duke/m4rom)); enables the SD-card emulation when M4 is toggled on |
| `AmstradDiagLower.rom` | Amstrad Diagnostics lower ROM (optional — enables Diag Cart toggle in the overlay) |

At runtime, ROMs are looked up in this order:

1. `~/.config/1984/roms/<file>` (user override)
2. `$(datadir)/1984/roms/<file>` (system install)
3. `./roms/<file>` (dev tree)

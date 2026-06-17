# Building FUZIX for the Amstrad CPC from source

This is the working recipe for building ajcasado's FUZIX port (the `cpcsme`
platform — CPC with Standard Memory Expansion) from a clean checkout, on
Fedora 44 inside our `my-distrobox` container.

Upstream: https://github.com/ajcasado/FUZIX

## One-time setup

Everything below runs inside `my-distrobox`. Enter it once per shell:

```bash
distrobox enter my-distrobox
```

### 1. Cross-compiler

SDCC is already installed at `~/Dev/sdcc/bin/sdcc` (v4.5.24 or later — anything
recent works). Make sure it's on PATH:

```bash
sdcc --version | head -1   # SDCC ... 4.5.24 ...
```

### 2. CPC packaging tools — not in Fedora repos, build from source

#### cpctools (`createSnapshot`, `cpcfs`, etc.)

```bash
sudo dnf install -y libdsk-devel
cd ~/Dev
git clone https://github.com/cpcsdk/cpctools.git
cd cpctools/cpctools
mkdir -p build && cd build
cmake .. && make -j$(nproc)
sudo make install
# /usr/local/lib isn't on the default linker path on Fedora — add it once:
echo /usr/local/lib | sudo tee /etc/ld.so.conf.d/local.conf
sudo ldconfig
createSnapshot 2>&1 | head -1   # banner = installed correctly
```

#### hex2bin (Intel HEX → raw binary)

```bash
cd ~/Dev
git clone https://github.com/algodesigner/hex2bin.git
cd hex2bin && make
sudo cp hex2bin /usr/local/bin/
```

#### iDSK (CPC .dsk image manipulation)

```bash
cd ~/Dev
git clone https://github.com/cpcsdk/idsk.git
cd idsk
cmake . && make
sudo cp iDSK /usr/local/bin/
```

#### `flip` — shim around `unix2dos`

The original `flip` utility isn't in any public repo we could find. The
FUZIX Makefile only uses `flip -m`, which converts a text file to MS-DOS
(CR/LF) line endings — `unix2dos` does exactly that.

```bash
sudo dnf install -y dos2unix
sudo tee /usr/local/bin/flip > /dev/null << 'EOF'
#!/bin/sh
# flip shim for FUZIX builds — only -m is needed (MS-DOS line endings).
if [ "$1" = "-m" ]; then
    shift
    for f in "$@"; do unix2dos -q "$f"; done
else
    echo "flip shim: only -m supported" >&2
    exit 1
fi
EOF
sudo chmod +x /usr/local/bin/flip
```

### 3. Writable build prefix

FUZIX's top-level Makefile **hardcodes** `/opt/fcc` as the install prefix for
the Library headers. There's no override variable. Make it owned by you:

```bash
sudo mkdir -p /opt/fcc
sudo chown $USER:$USER /opt/fcc
```

## Clone FUZIX

```bash
cd ~/Dev
git clone https://github.com/ajcasado/FUZIX.git
```

## Building the kernel only (what 1984 actually needs)

The full top-level `make TARGET=cpcsme` tries to build all userspace
(Library, Applications) too — that requires `fcc` from the FuzixCompilerKit,
which is a separate project and not strictly needed for the kernel.

For the kernel by itself, build directly from `Kernel/` with three vars set:

```bash
cd ~/Dev/FUZIX/Kernel
VERSION=0.5 SUBVERSION=1 TARGET=cpcsme FUZIX_ROOT=$HOME/Dev/FUZIX make
```

- `VERSION` / `SUBVERSION` come from `version.mk` at the top level — when
  building from `Kernel/` directly they aren't included, so we set them
  explicitly. Without them the `makeversion` step errors out.
- `FUZIX_ROOT` is referenced by the cpcsme `rules.mk` to find the peephole
  optimizer config. Without it sdcc fails with "cannot open peep rule file".

Build outputs:

| File | Purpose |
|---|---|
| `Kernel/fuzix.bin` | Packed RAM image |
| `Images/cpcsme/fuzixcpc.sna` | 128 KB snapshot — load directly in 1984 / Caprice32 / WinAPE |
| `Images/cpcsme/fuzix.dsk` | Boot floppy — `RUN"FUZIX"` from BASIC |

## Switching peripheral modes

The cpcsme defaults assume a real board layout. Edit
`Kernel/platform/platform-cpcsme/config.h` to flip modes, then `make clean`
and rebuild:

| Variable | Default | What it enables |
|---|---|---|
| `CONFIG_USIFAC_CH376` | **on** | USIfAC's on-board CH376 as USB mass storage |
| `CONFIG_USIFAC_SERIAL` | off (forced off by CH376) | USIfAC as a serial TTY (`/dev/tty3`) |
| `CONFIG_USIFAC_SLIP` | off | SLIP networking on top of `_SERIAL` |
| `CONFIG_ALBIREO` | on | Albireo CH376 USB host |
| `CONFIG_TD_IDE` | on | SYMBiFACE II / Cyboard IDE |

CH376 and SERIAL are mutually exclusive on the USIfAC pin out — `config.h`
auto-undefs `CONFIG_USIFAC_SERIAL` when `CONFIG_USIFAC_CH376` is defined.
To use the USIfAC as a TTY:

```c
#define CONFIG_USIFAC_SERIAL
/* ... */
#undef CONFIG_USIFAC_CH376
```

Then run a clean rebuild:

```bash
cd ~/Dev/FUZIX/Kernel && make clean
VERSION=0.5 SUBVERSION=1 TARGET=cpcsme FUZIX_ROOT=$HOME/Dev/FUZIX make
```

Confirm the right paths got compiled in:

```bash
strings ~/Dev/FUZIX/Kernel/fuzix.bin | grep -i usifac
# CH376 mode prints "...CH376 module serial comunication configured at 1MBPS"
# SERIAL mode prints "Usifac serial port configured at 115200 baud"
```

The TTY appears as `/dev/tty3`. To use it as a login console, edit
`/etc/inittab` on the running system and change:

```
03:3:off:getty /dev/tty3
```

to

```
03:3:respawn:getty /dev/tty3
```

## Running in 1984

Boot path 1 — **snapshot**:

```bash
./1984 --autostart=snapshot --sna=$HOME/Dev/FUZIX/Images/cpcsme/fuzixcpc.sna
```

The snapshot is 128 KB; the kernel prints `WARNING: Increase PTABSIZE` at
boot but still runs. Prefer the floppy path for any real work.

Boot path 2 — **floppy**, the way FUZIX is meant to be used:

```bash
./1984 \
  --memory=512 \
  --disk-a=$HOME/Dev/FUZIX/Images/cpcsme/fuzix.dsk \
  --autostart=fuzix
```

At the `bootdev:` prompt enter `hda1` (SYMBiFACE IDE), `hda2` (Albireo CH376),
or whichever device shows up first — depends on which peripheral toggles are
on in the 1984 config and which `disk.img` you mounted. Then log in as `root`
(no password).

For more context on the 1984 side of this work (CRTC fixes, port-decode
fixes, CH376 INT-handshake fix), see [issue-62-fuzix-notes.md](issue-62-fuzix-notes.md).
For USIfAC RS232 specifics, see [USIFAC.md](USIFAC.md).

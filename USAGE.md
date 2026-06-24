# Using 1984

## Command line

```bash
1984 [OPTIONS]
```

| Option | Description |
|--------|-------------|
| `--464` | Boot as CPC 464 (overrides config) |
| `--664` | Boot as CPC 664 (overrides config) |
| `--6128` | Boot as CPC 6128 (overrides config) |
| `--dd1` | Enable DDI-1 floppy interface on CPC 464 (overrides config) |
| `--memory=KB` | RAM size: 64, 128, 256, 512 or 576 (overrides config) |
| `--disk-a=PATH` | Mount a DSK image in drive A (overrides config) |
| `--disk-b=PATH` | Mount a DSK image in drive B (overrides config) |
| `--rom-slot=N:PATH` | Load a ROM image into upper ROM slot N (0-31); may be repeated |
| `--rom-os=PATH` | Override the lower ROM (OS) with a custom image at PATH |
| `--autostart=NAME` | After boot, types `run"NAME` into BASIC |
| `--paste=TEXT` | After boot, types TEXT verbatim (`\n` becomes Enter) |
| `--screenshot-at=N:PATH` | Save a screenshot at frame N to PATH, then exit |
| `--monitor-pty` | Open a PTY for the memory monitor (`minicom -b 9600 -D <path>`) |
| `--trace-io` | Log CRTC and Gate Array register writes to stderr |
| `--trace-palette` | Log palette writes and the firmware-flush fallback to stderr |
| `--trace-input` | Log keyboard and joystick events to stderr |
| `--trace-m4` | Log every M4 board command/response to stderr (M4 emulation is unstable) |
| `--trace-albireo` | Log every Albireo (CH376) command/response to stderr |
| `--trace-net4cpc` | Log every Net4CPC (W5100S) register read/write and socket command to stderr |
| `--printer-pdf=DIR` | Capture parallel-printer output (`&EFxx`) to timestamped PDFs in DIR |
| `--printer-real` | Spool each captured page to the host's default CUPS printer via `lp` |
| `-h`, `--help` | Print this option summary and exit |

Passing an unrecognised option prints the usage summary to stderr and exits with code 1. The machine model can also be selected via the options overlay (F9).

### Examples

```bash
# Boot with a disk mounted in drive A
1984 --disk-a=game.dsk

# Autostart a specific file from the disk
1984 --disk-a=game.dsk --autostart=game

# Run a disk-based game that needs its own loader command
1984 --disk-a=game.dsk --paste='|disc\nrun"disc'

# Load a toolkit ROM into slot 5 at startup
1984 --rom-slot=5:~/.config/1984/roms/TOOLKIT.ROM

# Multiple ROM slots can be specified
1984 --rom-slot=5:~/.config/1984/roms/TOOLKIT.ROM --rom-slot=8:~/.config/1984/roms/OTHER.ROM

# Boot FUZIX (ajcasado port). Needs 6128 + ≥512 KB + SymbIface IDE pointed at disk.img;
# at the bootdev: prompt type hda1, then log in as root.
1984 --model=6128 --memory=512 \
     --disk-a=~/Downloads/fuzix-cpc/fuzix.dsk \
     --disk-b=~/Downloads/fuzix-cpc/root.dsk \
     --autostart=fuzix

# FUZIX also boots from an Albireo (CH376) USB image — point albireo_image at
# the same disk.img in ~/.config/1984/1984.conf and FUZIX registers it as hda
# (hda1–hda4). Keep a pristine copy of disk.img and work on a copy; FUZIX
# filesystems are easy to dirty if the guest isn't shut down cleanly.
```

## Keyboard shortcuts

| Key      | Action |
|----------|--------|
| F4       | Save screenshot (`<binary>_<timestamp>.ppm`) and play camera shutter sound |
| F5       | Warm reset |
| F6       | Toggle GIF screen recording (auto-named `1984-<timestamp>.gif` in CWD) |
| F8       | Open/close memory monitor / debugger |
| F9       | Open/close options overlay |
| F10      | Mount the active card images (M4 SD / IDE / Albireo) on the host and open the file manager; press again to unmount and cold-boot. **Linux only**, needs `libguestfs-tools` (`guestmount`, `guestunmount`) and `xdg-utils` installed. |
| F11      | Toggle fullscreen |
| F12      | Quit |
| Ctrl+V   | Paste clipboard text into the emulator |
| Ctrl + + | Enlarge the window by one scale step (1× CPC native … 4×) |
| Ctrl + − | Shrink the window by one scale step |

A centred footer strip just above the LED bar shows the current model (`CPC 6128` / `CPC 664` / `CPC 464` in bold red) and the F-key hints, so you never need to leave the running emulator to remember a shortcut. The footer is drawn on top of the CPC display each frame and is visible in fullscreen too. The OS window title is just `1984`.

### F10 — browse card images on the host

Pressing **F10** pauses the emulated CPC and mounts every active card image (M4 SD / IDE / Albireo) on the host, then opens the file manager at the mount root. Drag files in or out as you would with any folder.

To finish browsing: either **press F10 again**, or **eject the card from the file manager** (the eject button next to the mounted volume in Nautilus' sidebar). Either way 1984 unmounts, syncs, and cold-boots so the guest re-reads the FAT cleanly. Cold-boot is mandatory because the guest's in-RAM FAT cache would otherwise overwrite your changes on the next sync.

Mount backend cascade (best path wins per card):

1. **`gnome-disk-image-mounter`** if present (modern GNOME). Mount lands under `/run/media/$USER/<label>/` as a first-class removable volume that Nautilus / Files treat natively — drag/drop just works.
2. **`udisksctl loop-setup + mount`** for non-GNOME desktops (KDE / XFCE). Same end result, no GTK dialog.
3. **`guestmount`** (libguestfs) as last resort. Mount lives in `~/.cache/1984/mounts/`. GNOME Files refuses drag/drop into FUSE mounts so this tier is browse-from-CLI only.

Floppies (`.DSK`) are not supported (AMSDOS filesystem, not FAT). M4 in directory mode (`m4_path` set, no `m4_image`) is skipped since the host directory is already directly accessible.

Install:
- Fedora: `sudo dnf install udisks2 gnome-disk-utility libguestfs-tools xdg-utils`
- Debian/Ubuntu: `sudo apt install udisks2 gnome-disk-utility libguestfs-tools xdg-utils`

If no backend is available, F10 logs to stderr and is otherwise a no-op.
| Ctrl+Enter | Release captured mouse (if SYMBiFACE mouse is active) |

## Joystick / gamepad

Any USB or Bluetooth controller recognised by SDL3 is automatically mapped to CPC joystick 1 (keyboard matrix row 9). Hot-plug is supported — controllers can be connected or disconnected at any time.

| Controller input | CPC joystick |
|---|---|
| D-pad or left stick | Up / Down / Left / Right |
| South button (A / Cross) | Fire 1 |
| East / West / North buttons | Fire 2 |

Controllers that SDL3 recognises via its gamepad database are opened as gamepads; any other device (legacy sticks, budget pads not in the database) falls back to raw joystick mode and is mapped by physical axis/button index (axis 0 = left/right, axis 1 = up/down; button 0 = Fire 1, button 1 = Fire 2; hat switch also works).

Gamepad and joystick events are delivered regardless of whether the emulator window has focus (`SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` is enabled at startup).

## Screen / video capture

Two paths, picked by what you want from the output.

| Action | Output | When to use |
|--------|--------|-------------|
| **F4** | `.ppm` (single frame) | Bug reports, regression baselines |
| **F6** | `.gif` (animated) | Quick share, no dependencies, ≤ few seconds of gameplay |
| **F9 → Advanced → Capture video** | `.webm` (VP9) | Longer clips, YouTube uploads, anything where GIF size becomes painful |

Both video paths capture the CPC framebuffer **before** the overlay draws, so the recording shows only the CPC screen. Output is scaled to 768×576 (correct 4:3 — the CPC framebuffer is 768×272 with non-square pixels).

**GIF (F6)** uses an in-tree GIF89a + LZW encoder — no codec libraries, no ffmpeg required. Captured at 25 fps. Lossless palette for the CPC's ≤27 simultaneous colours. Toggle F6 again to stop; the file is finalised on stop or on clean exit.

**WebM (overlay)** spawns `ffmpeg` as a subprocess and pipes raw frames to it; `ffmpeg` does the VP9 encoding (50 fps, 2 Mbit/s) and Matroska muxing. `./configure` detects ffmpeg at build time; on systems where it was absent the overlay row shows `[needs ffmpeg — F6 still records .gif]`. The encoded bitstream is YouTube-ingestible without re-encoding.

## Paste from host (Ctrl+V)

Pressing Ctrl+V types the host clipboard contents into the emulator one character at a time, simulating keypresses through the CPC keyboard matrix. Useful for entering BASIC programs. Supports letters, digits, common punctuation, and newlines. Each pasted block ends with an automatic Return.

## Options overlay (F9)

The overlay lets you change the machine model, RAM size, ROM paths, and hardware options without editing the config file. Navigate with arrow keys, press Enter to cycle a value. On close, if anything changed you will be asked whether to save.

Switching the model automatically sets the matching ROM paths and RAM size.

**RAM size** (General → Memory): press Enter to cycle through 64, 128, 256, 512, 576, 768, and 1024 KB. Up to 576 KB uses DK'tronics-compatible banking (Gate Array port 0x7Fxx, data bits[5:3] select the 64 KB bank group). 768 KB and 1024 KB switch to the Yarek/RAM7 extended scheme, where port address bits A10–A8 carry an additional bank group selector: port 0x7Exx adds a second 512 KB block (576–1088 KB range), giving a practical ceiling of 1024 KB with bank_high values 0–1. Banking is supported on all three models. Changing RAM size triggers a cold boot on save.

**CPC 464 and DD1:** on the 464, the Media tab drives are greyed out by default. Enable **DD1** in the Extensions tab to activate the DDI-1 floppy interface — this enables drive access and loads AMSDOS into ROM slot 7. On the **664 and 6128** the FDC and AMSDOS are built in: drives are always enabled and the DD1 row reads "N/A (built-in FDC)".

**Tape** (Media → Tape): selects a `.cdt` (TZX) cassette image. The decoder supports the common TZX block types (`0x10` standard speed data, `0x11` turbo data, `0x12` pure tone, `0x13` pulse sequence, `0x14` pure data, `0x20` pause) plus the metadata blocks (`0x21`–`0x34`, `0x5A`) which are skipped. Pulse timings are scaled from Spectrum 3.5 MHz to CPC 4 MHz (40/35 ratio). The cassette data line is OR'd into PPI Port B bit 7; PPI Port C bit 4 acts as the motor enable. While the motor is on, the level is also sampled at audio rate (~90 cycles/sample at 4 MHz / 44.1 kHz) and mixed into the PSG output at ±2500 amplitude — you hear the loading screech exactly as on real hardware.

On the **CPC 464** the tape is always wired (the deck is built in). On the **664 and 6128** there's no built-in deck, so an **External Tape** toggle appears in the General tab (between Roms Board and OS ROM, only visible on disk machines); enable it to virtually plug the cassette deck into the tape port. Toggling triggers a cold boot so the firmware re-probes.

**MX4** (General → MX4): toggles the CPC's MX4 expansion connector. When `enabled` (the default), expansion peripherals on the Extensions tab are available; when `disabled`, every extension I/O port (`0xFDxx`, `0xFExx`, `0xFFxx`) returns `0xFF` as if nothing were plugged in, and the Extensions tab is hidden from the overlay. The toggle triggers a cold boot on save so the CPC firmware re-probes the bus. Useful for testing whether a guest application depends on a peripheral, or running an OS that misbehaves when it sees one.

**Roms Board** (General → Roms Board): toggles whether the 32 expansion ROM slots are populated at boot. When `enabled` (default), every non-empty `slot_N=` entry in `1984.conf` is loaded into the matching upper ROM slot and the Extensions → ROM Slots sub-panel is active. When `disabled`, only the three standard ROMs for the model — OS + BASIC + AMSDOS — are loaded; the `slot_N=` entries are kept in the config untouched, so re-enabling the toggle restores the previous layout from a single source of truth. Triggers a cold boot on save.

Because **M4**, **SYMBiFACE IDE**, **SYMBiFACE Mouse**, and **Albireo** install their drivers as upper-ROM cartridges, they cannot function without the Roms Board. When Roms Board is `disabled`, those rows in the Extensions tab show `[needs Roms Board]` and refuse to toggle; the live CPC state forces them off too (their cfg values are preserved). **Net4CPC** and **RTC** are bare-hardware peripherals that don't need any companion ROM and remain available either way.

**OS ROM / BASIC ROM** (General → OS ROM, General → BASIC ROM): press Enter on either row to open a file picker and select a different ROM image. Changing either triggers a cold boot on save so the new ROM is in effect from the next reset. The values shown next to each row are the basename of the currently-configured ROM.

**ROM Slots** (Extensions → ROM Slots) opens a sub-panel listing the lower ROM and all 32 upper ROM slots (0–31):

| Entry | Default | Enter | Delete |
|-------|---------|-------|--------|
| Lower ROM | Model OS ROM | Replace with file picker | Restore model default |
| Slot 0 | BASIC ROM | Load expansion override | Clear override / restore default BASIC |
| Slot 7 | AMSDOS ROM | Load expansion override | Clear override / restore default AMSDOS |
| Slots 1–6, 8–31 | empty | Load ROM into slot | Clear slot |

**Diagnostics Cartridge** (Extensions → Diag Cart): toggles the lower ROM between the model's default OS and `AmstradDiagLower.rom`. When ON, the machine boots into the Amstrad Diagnostics program. When OFF, the lower ROM reverts to the model's normal OS ROM. The toggle is greyed out if `AmstradDiagLower.rom` is not found in the ROMs directory. The change triggers a cold boot on save.

**Net4CPC** (Extensions → Net4CPC): enables emulation of the Net4CPC Ethernet add-on board based on the WIZnet W5100S chip. When enabled, four I/O ports are exposed at 0xFD20–0xFD23:

| Port | Name | Description |
|------|------|-------------|
| 0xFD20 | MR | Mode Register — reads 0x03 when the chip is present |
| 0xFD21 | IDM_ARH | High byte of the 16-bit indirect address register |
| 0xFD22 | IDM_ARL | Low byte of the 16-bit indirect address register |
| 0xFD23 | IDM_DR | Data register — read/write to the W5100S register space at the current address; auto-increments when MR bit 1 (AI) is set |

Socket operations (TCP connect/send/receive, UDP sendto) are backed by host POSIX sockets. Four sockets (0–3) are available, each with 2 KB TX and 2 KB RX ring buffers. This is compatible with the Z80 driver in the [N4C-NETTOOLS](https://github.com/salvogendut/n4c-nettools) library and with the SymbOS N4C network daemon. The toggle triggers a cold boot on save.

**Net4CPC TAP** (Advanced → Net4CPC TAP, Linux only): swaps the legacy host-socket backend for a real L2 endpoint on a kernel TAP device. The W5100S is no longer just a thin shim around POSIX sockets — outbound frames are assembled as real Ethernet + IP + UDP/TCP, ARP / ICMP / TCP are handled inside the emulator, and inbound frames are demultiplexed back to the matching W5100S socket. The CPC becomes pingable from the host, accepts inbound connections, and **DHCP works**.

When enabled, 1984 auto-provisions everything via a single `pkexec` prompt: creates `cpc-tap0`, assigns the configured host IP, adds the device to firewalld's trusted zone, enables IPv4 forwarding, and installs narrow MASQUERADE + FORWARD rules so the CPC can reach the wider network through the host. An in-process DHCP server hands out the configured lease range; an in-process DNS proxy forwards to the host's `/etc/resolv.conf` upstream. The tap device is reused across 1984 launches and only re-created when the configured subnet changes or the host reboots — so you get **one polkit prompt per host uptime**, not per launch.

**Pick a subnet that doesn't collide with your LAN.** Defaults to `10.0.0.0/24`. If your home Wi-Fi or router already uses that range, edit `~/.config/1984/1984.conf` and change the four keys before enabling TAP:

```ini
net4cpc_tap_host_ip=192.168.99.1
net4cpc_tap_netmask=255.255.255.0
net4cpc_tap_lease_start=192.168.99.100
net4cpc_tap_lease_end=192.168.99.150
```

KCNet utilities reject non-RFC1918 IPs; stay inside `10.0.0.0/8`, `172.16.0.0/12`, or `192.168.0.0/16`. See [NET4CPC.md](NET4CPC.md) for the full setup walkthrough and the power-user `--tap=DEVNAME` CLI for managing your own tap.

For debugging, `--trace-net4cpc` logs every W5100S register read/write (decoded with register names and socket index), every socket command (`OPEN`, `CONNECT`, `SEND`, `RECV`, `CLOSE`), and TX/RX buffer access summaries. `--trace-tap` logs the L2/L3 events (ARP, IPv4, UDP, ICMP, TCP) on the TAP backend.

**RTC** (Extensions → RTC): enables emulation of a DS12887 real-time clock compatible with the Cyboard and Symbiface II add-on boards. Time is sourced from the host OS via `localtime()` and is always current. Two I/O ports are exposed at:

| Port | Direction | Description |
|------|-----------|-------------|
| 0xFD15 | write | Address register — selects which DS12887 register (0x00–0x7F) to access |
| 0xFD14 | read/write | Data — read or write the selected register |

Time and date registers (seconds, minutes, hours, day-of-week, day, month, year, century) always reflect the current host time. Register B controls binary/BCD and 12h/24h format. The 114-byte NVRAM area (registers 0x0E–0x7F) is stored in RAM for the lifetime of the session. SymbOS detects the RTC via its `SYMBOS.INI` hardware flags byte (offset 0x240, bit 1) and reads the clock at boot using binary 24h mode. The toggle does **not** trigger a cold boot.

**SYMBiFACE IDE** (Extensions → SYMBiFACE IDE): enables emulation of the SYMBiFACE II / Cyboard compatible IDE interface. The backend is a raw disk image file formatted with FAT16 or FAT32 (`.img`). Enabling the option opens a file picker to select the image; the path is saved to `ide_image` in `1984.conf`. The following I/O ports are emulated:

| Port | Direction | Description |
|------|-----------|-------------|
| 0xFD06 | read/write | Alternate Status (read) / Device Control (write) |
| 0xFD08 | read/write | Data |
| 0xFD09 | read/write | Error (read) / Features (write) |
| 0xFD0A | read/write | Sector Count |
| 0xFD0B | read/write | LBA Low |
| 0xFD0C | read/write | LBA Mid |
| 0xFD0D | read/write | LBA High |
| 0xFD0E | read/write | Device / Head |
| 0xFD0F | read/write | Status (read) / Command (write) |

Supported ATA commands: IDENTIFY DEVICE (0xEC), READ SECTORS (0x20/0x21), WRITE SECTORS (0x30/0x31), INITIALIZE DRIVE PARAMETERS (0x91), SET FEATURES (0xEF). Multi-sector transfers and software reset (SRST via Device Control) are supported. The open image file is preserved across warm resets (F5); only a cold boot closes and reopens it. Enabling, disabling, or changing the image triggers a cold boot.

**SYMBiFACE Mouse** (Extensions → SYMBiFACE Mouse): enables emulation of the SYMBiFACE II PS/2 mouse interface at port 0xFD10. When enabled, clicking inside the emulator window captures the host mouse (cursor hidden, relative mode). While captured, host mouse movement and button presses drive the emulated PS/2 mouse. Press **Ctrl+Enter** to release the mouse; the window title shows the current capture state. The toggle triggers a cold boot on save so SymbOS's input drivers re-probe the hardware. Immediately usable by SymbOS without any additional drivers. Mouse capture is gated on this toggle — enabling Albireo on its own does not engage capture.

The port returns a variable-length burst of packets terminated by `0x00` (no more data). Only fields that actually changed are included in each burst:

| Byte | Meaning |
|------|---------|
| `0x00` | No more data — stop reading |
| `0x40`–`0x7F` | X offset, signed 6-bit; positive = right |
| `0x80`–`0xBF` | Y offset, signed 6-bit; positive = up |
| `0xC0`–`0xDF` | Button state: bit0=left, bit1=right, bit2=middle |
| `0xE0`–`0xFF` | Scroll wheel offset, signed 5-bit |

**Albireo** (Extensions → Albireo): enables emulation of the Albireo CPC expansion board, which exposes a WCH **CH376** USB host controller at I/O ports `0xFE80` (data) and `0xFE81` (command/status). The backend is a FAT16/FAT32 image file mounted via `src/fat.c`; the chip's built-in file-system command set is emulated against that image, so guest software talks to the chip exactly as it would to a real Albireo with a USB drive plugged in. Enabling the option opens a file picker to select the image; the path is saved to `albireo_image` in `1984.conf`.

Albireo is **mutually exclusive with the M4 board** — both expansions decode the `0xFExx` port range — so enabling one disables the other and clears its image path.

Albireo is primarily designed to be driven by the [UNIDOS](https://unidos.cpcscene.net/) ROM (and its `Albireo.rom` DOS node). Minimum install:

| ROM slot | ROM |
|----------|-----|
| 7 | `UNIDOS.ROM` (replaces AMSDOS — load it into slot 7 via the ROM Slots overlay so the firmware sees it first) |
| 8 | `ALBIREO.ROM` (the Albireo DOS node) |
| 9 | `UNITOOLS.ROM` (optional but recommended; provides `|U`, `|DRIVE`, etc.) |

ROM files distributed as AMSDOS-headed binaries (16384 + 128 bytes = 16512 bytes) are auto-stripped on load — no need to remove the header manually. This applies to **every** ROM loaded by the emulator (OS, BASIC, AMSDOS, expansion slots), not just Albireo, so Cyboard, M4, SYMBiFACE, etc. all benefit. With the above setup, `|DRIVE` shows the SD/USB drives, `|A` activates a drive letter, and `CAT` lists the FAT image's directory. NVRAM (the Albireo's SC16C650B-backed storage that UNIDOS uses) is currently emulated as a regular file (`!UNIDOS!.NVM`) at the FAT root, so UNIDOS reports "no NVRAM, degraded mode" but file operations work.

Implemented CH376 commands: `GET_IC_VER`, `RESET_ALL`, `CHECK_EXIST`, `SET_USB_MODE`, `GET_STATUS`, `RD_USB_DATA0`, `WR_REQ_DATA`, `SET_FILE_NAME`, `DISK_CONNECT`, `DISK_MOUNT`, `FILE_OPEN` (incl. wildcard enumeration), `FILE_ENUM_GO`, `FILE_CREATE`, `FILE_CLOSE`, `DIR_INFO_READ`, `BYTE_LOCATE`, `BYTE_READ` / `BYTE_RD_GO`, `BYTE_WRITE` / `BYTE_WR_GO`, `DISK_CAPACITY`, `DISK_QUERY`. Not implemented (yet): `FILE_ERASE`, `DIR_INFO_SAVE`, the SC16C650B UART side at `0xFEB0–7`, and CH376 interrupts routed to NMI (UNIDOS polls the status register, so this is fine).

**Cyboard** (Extensions → Cyboard): convenience toggle that enables or disables Net4CPC, RTC, SYMBiFACE IDE, and SYMBiFACE Mouse all at once. Shows `enabled` when all four are on, `disabled` when all four are off, and `partial` when mixed. Disabling also clears the IDE image path.

**USIfAC RS232** (Extensions → USIfAC RS232): enables wire-level emulation of the USIfAC II serial board at I/O ports `&FBD0..&FBDF`. The host-side endpoint is a PTY (default — shown as `PTY:/dev/pts/N` in the overlay) or a TCP listener (`TCP:4001`). Choose the backend from **Advanced → USIfAC mode** (Tinker must be enabled). Bytes written to `&FBD0` by the CPC emerge on the host endpoint; bytes sent in arrive on the CPC's RX FIFO and `INP(&FBD1)` flips to `0xFF` until they're read. FUZIX completes its `usifexists` / `usifgetbaud` handshake at boot (verified end-to-end). A new split LED in the activity bar shows RX traffic in red (host → CPC) and TX in green (CPC → host). See [docs/USIFAC.md](docs/USIFAC.md) for the full port map, control-byte table, and connection recipes.

**USIfAC PTY link** (Advanced → USIfAC PTY link): optional stable host-side symlink pointing at the live `/dev/pts/N` slave, so external tools (minicom, `tools/pty_modem.py`, custom scripts) can reconnect at a fixed path across launches without chasing the randomised pts number. Press Enter on the row to open an inline text editor seeded with the current value (empty if none); type the path (e.g. `/tmp/usifac.pty`), Enter to commit, Esc to cancel, Backspace to delete a character, Delete to clear the buffer. Committing an empty buffer removes the alias. The link is recreated on every USIfAC re-init and removed on shutdown (only if it still points at the slave we created). PTY backend only; ignored on Windows. Sets `usifac_pty_link=` in the `[hardware]` section of the config.

**PDF printer** (Extensions → PDF printer): captures parallel-printer output from the CPC's Centronics port (`&EFxx`) into timestamped PDFs on the host. Enter on the row pops a folder picker; the chosen directory persists across runs (`pdf_printer_dir` in `1984.conf`). Each printed page comes out as `1984-print-YYYYMMDD-HHMMSS.pdf` written ~2 seconds after the last byte (idle-finalise so the file is openable mid-job). `PRINT #8` / `LIST #8` / CP/M `LST:` / AMSDOS `|PRINTER` all route here automatically. Cairo is required for capture (`./configure --without-cairo` falls back to a no-op so the port is still decoded). The printer is gated on MX4 — disable the expansion bus and the row reads `[needs MX4]`.

**Printer mode** (Extensions → Printer mode): cycles the captured output between `PDF file` (kept in the directory above) and `Real printer (CUPS lp)`, which spools each finalised page to the host's default CUPS printer via `lp`. When 1984 runs inside a sandboxed environment, the spooler tries `distrobox-host-exec lp` and `flatpak-spawn --host lp` before falling back to plain `lp` so the host's CUPS stack is reachable without installing `cups-client` in the container. Sink changes take effect on the next character.

Changes to the model, RAM size, DD1 toggle, any ROM slot, lower ROM, SYMBiFACE IDE, SYMBiFACE Mouse, or Albireo image trigger an automatic cold boot so the new configuration takes effect immediately. The machine re-boots without needing to quit and restart.

## Memory monitor / debugger (F8)

Press **F8** to open a separate 80×25 green-phosphor terminal window. All commands are also available via a PTY serial port with `--monitor-pty`.

| Command | Description |
|---------|-------------|
| `D <addr> [<end>]` | Disassemble Z80 (10 lines default; pageable) |
| `M <addr> [<end>]` | Hex + ASCII dump (page default; ASCII in reverse video) |
| `B [<addr>]` | Set a breakpoint / list all breakpoints |
| `BC <n>` | Clear breakpoint slot n (0 – 15) |
| `S [<name>]` | Show the symbol+offset for the current PC, or jump-disassemble at `<name>` (requires `--symbols`) |
| `BS <name>` | Set a breakpoint at the address of `<name>` (requires `--symbols`) |
| `N` | Single-step one instruction (when paused) |
| `G` | Resume execution |
| `GA` | Gate Array: screen mode + all 16 inks |
| `CRTC` | All 18 CRTC registers + live counters |
| `X` / `Q` | Close monitor |

When a breakpoint fires the emulator freezes, the monitor opens automatically, and shows the hit address with a 5-line disassembly. A live register bar (`PC SP A F BC DE HL IX IY`) is always visible at the bottom of the window, turning red while paused.

```bash
# Connect to the monitor over serial with minicom
1984 --monitor-pty
minicom -b 9600 -D /dev/pts/N    # path printed to stderr at startup
```

See [Development.md](Development.md) for the monitor's internals and the breakpoint/pause data flow.

## Configuration file

On first run a configuration file is created at `~/.config/1984/1984.conf`. You can edit it directly or use the in-app options overlay (F9).

```ini
[machine]
model=6128        # 464, 664, or 6128
memory=128        # 64, 128, 256, 512, or 576 (KB); default 64 for 464/664, 128 for 6128

[roms]
os=~/.config/1984/roms/OS_6128.ROM
basic=~/.config/1984/roms/BASIC_1.1.ROM
amsdos=~/.config/1984/roms/AMSDOS.ROM   # 664/6128; cleared automatically on 464 unless DD1 is on

[expansion_roms]
# Load extra ROMs into upper ROM slots 0-31.
# Slot 0 = BASIC fallback, slot 7 = AMSDOS fallback; all slots can be overridden.
# Example: slot_5=~/.config/1984/roms/TOOLKIT.ROM

# Per-board conf templates — when a board is enabled in [hardware] below,
# the slot paths AND the image path listed under its [board:NAME] section
# are loaded into the live config automatically; when the board is
# disabled, the live slots clear but the [board:NAME] template stays
# parked for the next enable (no re-prompt for the image path).
# Whitelisted board names: m4, albireo, cyboard. Slot tags via the
# overlay (Options → Extensions → ROM Slots → Ins on a populated slot).
# The image is captured automatically the first time you pick a disk
# image for that board. Press Del on the M4 / Symbiface IDE / Albireo
# row in the Extensions tab to clear the cached image. The same ROM
# can belong to multiple boards (multi-board coexistence). Conflict
# resolution: last-active-board wins, stderr warning names the slot.
# [board:cyboard]
# slot_1=~/.config/1984/roms/HDCPM.ROM
# image=~/Disks/cpcplus.img
# [board:albireo]
# slot_7=~/.config/1984/roms/UNIDOS.ROM
# image=~/Disks/usb-stick.img

[hardware]
mx4=true          # MX4 expansion bus — when false, every extension peripheral
                  # below (M4, Net4CPC, RTC, SYMBiFACE, Albireo, …) is
                  # disconnected and the Extensions overlay tab is hidden.
rom_board=true    # Expansion ROM board fitted — when false, only OS + BASIC +
                  # AMSDOS load at boot. slot_N entries above stay in the
                  # config but are ignored until re-enabled.
dd1=false         # CPC 464 only — DDI-1 floppy interface (enables drives + AMSDOS in slot 7)
m4=false          # M4 board emulation — file API + ESP8266 networking. UNSTABLE
ulifac=false      # [unimplemented]
net4cpc=false
net4cpc_tap=false                       # Linux TAP backend (auto-tap + built-in DHCP + DNS proxy + NAT)
net4cpc_tap_host_ip=10.0.0.1            # Host side of the tap interface
net4cpc_tap_netmask=255.255.255.0
net4cpc_tap_lease_start=10.0.0.100      # DHCP lease range — pick something
net4cpc_tap_lease_end=10.0.0.150        # that doesn't collide with your LAN
rtc=false         # DS12887 real-time clock (Cyboard/Symbiface II compatible)
symbiface_ide=false
ide_image=        # path to a raw FAT16/FAT32 disk image (.img)
symbiface_mouse=false
albireo=false     # Albireo CPC expansion (CH376 USB host controller)
albireo_image=    # Path to FAT16/FAT32 image used as the USB drive backing
usifac=false      # USIfAC II RS232 serial interface (ports &FBD0..&FBDF)
usifac_backend=pty   # "pty" exposes /dev/pts/N; "tcp" listens on localhost
usifac_tcp_port=4001 # TCP listen port when backend=tcp
usifac_pty_link=     # optional stable symlink to /dev/pts/N (PTY backend only)

[display]
scale=1           # 1, 2, 3, or 4 — window starts at 768·scale × (576·scale + LED bar)
fullscreen=false
```

# USIfAC II — Serial interface emulation

1984 emulates the **wire-level RS232 surface** of the USIfAC II (John
Konstantopoulos, 2021). The board appears at ports `&FBD0..&FBDD`; any
CPC software written against the User's Guide v3.0 "Send & Receive
bytes" pattern will see a working serial port backed by a host-side
PTY or TCP listener.

## What we emulate

- Two-port byte pipe at `&FBD0` (data) / `&FBD1` (status/control).
- Status semantics: `INP(&FBD1)` returns `0xFF` when an RX byte is
  available, `0x01` when empty.
- Presence probe at `&FBD8`: returns `0x00` when enabled
  (FUZIX's `usifexists` check).
- Baud-code readback at `&FBDD`: reflects the last `OUT &FBD1, 10..23`
  written by the host.
- Control commands on `&FBD1` writes: reset (0), clear RX (1), burst on/off
  (2/3), baud-set (10..23), and a stub `|STAT` reply (30).
- RX queue large enough to mirror the 3100-byte firmware buffer.

## What we do not emulate

- The host-side USIfAC ROM (the `FATFS-FT.ROM` / `FATFS-P1.ROM` images),
  so the higher-level RSX commands (`|USB`, `|CAT`, `|CD`, `|FDC`, `|MG`,
  `|SET`, `|EN`, etc.) are not available.
- The board's on-board CH376 USB host. (1984's Albireo card already
  emulates a CH376; if you need USB mass storage in the CPC, use Albireo.)
- The board's built-in AMSDOS/PARADOS ROM emulation and 765 FDC
  emulation — those run inside the PIC firmware, on the other side of
  the serial pipe.
- USIfAC's ROM-slot select port (`&FBD2`) and the ROM/RAM banking on the
  larger ULIfAC board.

## Configuration

`~/.config/1984/1984.conf`:

```
usifac=true                # enable the device
usifac_backend=pty         # "pty" (default) or "tcp"
usifac_tcp_port=4001       # only used when backend=tcp
```

You can also toggle it from the in-emulator overlay (Esc → Hardware →
USIfAC). The overlay shows the live PTY device path or TCP listen port
when enabled.

Legacy `ulifac=true` in older configs is accepted with a deprecation
warning — please update the key.

## Connecting

### PTY backend (Linux / macOS)

When `usifac_backend=pty`, 1984 allocates a PTY at startup and prints
the slave name to stderr (also shown in the overlay):

```
usifac: PTY ready at /dev/pts/7
```

Connect any terminal program to that path:

```
minicom -D /dev/pts/7
# or
picocom -b 115200 /dev/pts/7
```

Baud rate from the host program is ignored (the virtual link has no
physical bit clock), but configure your terminal at 115200 to match
what FUZIX-and-friends will write to `&FBDD`.

### TCP backend (any platform that has sockets)

When `usifac_backend=tcp`, 1984 listens on `localhost:<usifac_tcp_port>`
(default 4001). One client at a time.

```
nc localhost 4001
# or
telnet localhost 4001
```

## Smoke tests from BASIC

```basic
10 ' Print whatever comes in on the serial port
20 WHILE INP(&FBD1)=1: WEND
30 PRINT CHR$(INP(&FBD0));
40 GOTO 20
```

```basic
10 ' Send the string "HELLO" out the serial port
20 a$="HELLO"
30 FOR i=1 TO LEN(a$): OUT &FBD0, ASC(MID$(a$,i,1)): NEXT
```

```basic
10 ' Confirm presence and current baud code
20 PRINT "exists byte:";INP(&FBD8)   ' 0 when board enabled, 255 when not
30 OUT &FBD1, 16                     ' set "115200" baud code
40 PRINT "baud code:";INP(&FBDD)     ' should print 16
```

## Reference

- *Amstrad CPC Serial Interface II (USIfAC II) — User's Guide v3.0*, John
  Konstantopoulos, July 2021.
- Board product page: <https://www.tindie.com/products/ikonsgr/usifac-usb-mass-storage-board-for-amstrad-cpc/>
- FUZIX upstream driver, used as a reference implementation, lives at
  `Kernel/platform/platform-cpcsme/{discard.c,devtty.c,plt_ch375.h}` in
  <https://github.com/ajcasado/FUZIX>.

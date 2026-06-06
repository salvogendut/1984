# Net4CPC (Wiznet W5100S) — TAP backend

1984 emulates the Net4CPC Ethernet board as accessed through I/O ports
`0xFD20`–`0xFD23` (W5100S in indirect parallel bus mode). Out of the box,
each W5100S socket is mapped onto a host POSIX `socket()` — outbound TCP
and unicast UDP work, but the W5100S cannot truly *be on the wire*: it
isn't pingable, can't accept inbound connections from the LAN, and
DHCP/ARP are not implemented.

With `--tap=DEVNAME`, 1984 binds the W5100S to a Linux **TAP device**.
Outbound TX is assembled as real Ethernet frames (Eth + IP + UDP/TCP),
ARP / ICMP / TCP are handled inside the emulator, and inbound frames
are demultiplexed back to the matching W5100S socket. The board then
behaves as a real L2 endpoint on whatever the TAP is bridged to.

## What works through TAP

- Outbound and inbound TCP (active connect, passive `LISTEN`, full
  state machine, retransmits, FIN graceful close)
- Outbound and inbound UDP (with the 8-byte W5100S UDP header
  synthesised on receive)
- ICMP echo reply (the CPC is `ping`-able from the host or LAN)
- ARP (responder + cache, gateway resolution for off-subnet)
- KCNet utilities (`NCFG`, `PING`, `NTIME`, `CPMNET`, …) with static
  IP configured via `NCFG.INI`
- SymbOS networking
- DHCP if the TAP is bridged to a network that has a DHCP server

Linux only for now. macOS/Windows fall back to the legacy host-socket
behaviour (the `--tap=` flag is silently ignored on those platforms).

## Quick smoke test — point-to-point (no LAN, no bridge)

The fastest way to verify the TAP backend is a private subnet between
the host and the emulator:

```bash
# One-time TAP creation. The `user $USER` clause makes the device
# persistent and owned by you, so 1984 can open it without root later.
sudo ip tuntap add dev tap0 mode tap user $USER
sudo ip link set tap0 up
sudo ip addr add 10.0.0.1/24 dev tap0

# Launch 1984 with Net4CPC enabled (1984.conf or overlay) and bind
# the W5100S to the TAP:
./1984 --tap=tap0
```

Inside the CPC, configure a matching static IP. The simplest way is
the maintainer's `NCFG.INI` workflow — copy a profile onto a disk
mounted as `B:` (or onto your HDCPM A0: area) and run `ncfg myprof`:

```ini
[MyProf]
-i:10.0.0.2
-m:255.255.255.0
-g:10.0.0.1
-d:10.0.0.1
```

Verify both directions:

```bash
# Host -> CPC
ping 10.0.0.2     # should get replies from the W5100S
```

From inside the CPC, `B:PING 10.0.0.1` should also reply.

Tear down (when you're done, or to recreate fresh):

```bash
sudo ip link del tap0
```

## LAN integration — bridge TAP to a real interface

To make the CPC appear on your actual home LAN (so other devices can
reach it, and so DHCP can hand it an address), put the TAP into a
bridge that also contains your physical interface.

```bash
# Find your physical interface name (e.g. enp3s0, eth0, wlp4s0…)
ip -o link show

# Create the bridge if you don't already have one. (If you use
# NetworkManager you may already have one; check with `ip link`.)
sudo ip link add name br0 type bridge
sudo ip link set br0 up

# Move the physical interface into the bridge. NOTE: this will drop
# the IP currently on the physical interface; either move the IP
# onto the bridge (`sudo ip addr add <yourip>/24 dev br0`) or rely
# on NetworkManager to reconfigure.
sudo ip link set enp3s0 master br0
sudo ip addr flush dev enp3s0

# Create the TAP and add it to the bridge.
sudo ip tuntap add dev tap0 mode tap user $USER
sudo ip link set tap0 up
sudo ip link set tap0 master br0

# Run 1984.
./1984 --tap=tap0
```

The CPC now shares your LAN. Either:

- Configure a static IP in your subnet via `NCFG.INI` (this is what
  SymbOS uses; the maintainer's `[jKC]` profile is a good template), or
- If your LAN has a DHCP server, NCFG's `-a:HOSTNAME` profile should
  obtain a lease.

## Permissions

The first TAP launch needs `CAP_NET_ADMIN` to call
`ioctl(TUNSETIFF)`. Three ways to satisfy this:

1. **Persistent TAP** (recommended; what the commands above do):
   `ip tuntap add ... user $USER` creates the device persistently and
   makes it owned by you, so 1984 opens it as a normal user.
2. **`setcap`** on the binary: `sudo setcap cap_net_admin+ep ./1984`.
   1984 then creates fresh TAPs on demand. Side effect: many distros
   refuse to honour `LD_PRELOAD`, `LD_LIBRARY_PATH`, etc. on
   `setcap`'d binaries, which may break some development workflows.
3. Run `1984` as root — discouraged.

## Troubleshooting

- **`tap: open /dev/net/tun: Permission denied`** — the `tun` module
  isn't loaded or `/dev/net/tun` isn't accessible. `sudo modprobe tun`
  and check `ls -l /dev/net/tun`.
- **`tap: TUNSETIFF '...': Operation not permitted`** — either the
  named device exists but is owned by someone else, or you're trying
  to create one without `CAP_NET_ADMIN`. Use the persistent-TAP path
  in the smoke-test recipe above.
- **`ping` from host returns nothing** — make sure the W5100S's SIPR
  is in the same subnet as the host's TAP IP, and that the TAP is
  `up`. `tcpdump -i tap0 -nn` will show whether frames are leaving
  the emulator.
- **NCFG still hangs** — check `--trace-net4cpc` output for the
  register access pattern. If it's spinning on `Sn_IR` with status
  `0x22` (UDP), the program is waiting for a DHCP reply that won't
  come on a point-to-point setup; switch to a bridged setup or use a
  static-IP `NCFG.INI` profile instead.

## Tracing

Two independent trace flags help diagnose problems:

- `--trace-net4cpc` — every W5100S register read/write at the
  `0xFD20`–`0xFD23` port boundary, plus a one-line summary of each
  Sn_CR socket command. Useful when you don't trust the kernel's
  register-level handshake.
- `--trace-tap` — Ethernet/IP/UDP/TCP/ICMP/ARP frame events at the
  stack level (this is what you get when `--tap=` is in use).
  Sample output:

  ```
  [n4c] ARP -> who-has 192.168.68.1 tell 192.168.68.254
  [n4c] ARP <- reply  192.168.68.1 is-at aa:bb:cc:dd:ee:ff
  [n4c] TCP[0] -> 192.168.68.10:23 [S] seq=… ack=0 0 bytes
  [n4c] TCP[0] state SYN_SENT -> ESTABLISHED
  [n4c] UDP -> 8.8.8.8:53 from :49152  32 bytes
  [n4c] ICMP -> echo reply to 192.168.68.50 (60 bytes)
  ```

Combine both (`--trace-net4cpc --trace-tap`) to follow a packet from
the kernel writing TX bytes through the W5100S TX buffer, the stack
assembling it into IP, and the resulting Ethernet frame leaving the
TAP.


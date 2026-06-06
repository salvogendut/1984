/* tap.h — Linux TAP device backend for Net4CPC.
 *
 * Opens /dev/net/tun with IFF_TAP|IFF_NO_PI so we get raw Ethernet frames
 * (no Linux 4-byte protocol header). The W5100S behaves as the NIC of the
 * CPC and we (the emulator) bridge its TX/RX paths onto this TAP device,
 * so the chip really appears as an L2 endpoint on whatever the host bridges
 * the TAP to.
 *
 * All functions are Linux-only; on other platforms they're stubbed to
 * return -1 so callers fail closed.
 *
 * Frame sizes are bounded by ETH_FRAME_LEN_MAX. The W5100S has 2 KB TX
 * buffers per socket which already constrain payload; the extra headroom
 * here is for the Ethernet + IP + UDP/TCP overhead our stack adds.
 */
#pragma once
#include "types.h"
#include <stdbool.h>
#include <stddef.h>

#define TAP_FRAME_MAX  2048

/* Open the TAP device. Returns fd >= 0 on success, -1 on failure with errno
 * set. devname is the requested interface name; if NULL or empty, lets the
 * kernel pick one (tap0, tap1, …). The chosen name is written back into
 * out_name (size out_name_sz, including NUL terminator). */
int tap_open(const char *devname, char *out_name, size_t out_name_sz);

/* Close the TAP device. Safe with fd == -1. */
void tap_close(int fd);

/* Non-blocking read of one Ethernet frame. Returns:
 *   > 0  number of bytes read (frame length)
 *     0  no frame available right now
 *   -1   read failure (errno set; caller may close + reopen)
 */
int tap_read(int fd, u8 *buf, size_t maxlen);

/* Write one Ethernet frame. Returns bytes written on success, -1 on error. */
int tap_write(int fd, const u8 *buf, size_t len);

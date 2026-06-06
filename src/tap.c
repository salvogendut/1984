/* tap.c — Linux TAP device backend. See tap.h. */
#include "tap.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

#if defined(__linux__)
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>

int tap_open(const char *devname, char *out_name, size_t out_name_sz) {
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "tap: open /dev/net/tun: %s\n", strerror(errno));
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    /* IFF_TAP for Ethernet frames; IFF_NO_PI to skip the 4-byte Linux
     * protocol header so we deal in pure Ethernet, matching what the
     * W5100S would put on the wire. */
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    if (devname && devname[0])
        snprintf(ifr.ifr_name, IFNAMSIZ, "%s", devname);

    if (ioctl(fd, TUNSETIFF, &ifr) < 0) {
        fprintf(stderr, "tap: TUNSETIFF '%s': %s\n",
                ifr.ifr_name[0] ? ifr.ifr_name : "(auto)",
                strerror(errno));
        if (errno == EPERM)
            fprintf(stderr,
                "tap: CAP_NET_ADMIN required. Either run 1984 as root,\n"
                "tap: grant the binary 'setcap cap_net_admin+ep ./1984',\n"
                "tap: or pre-create a persistent TAP and chown it to you:\n"
                "tap:   sudo ip tuntap add dev tap0 mode tap user $USER\n"
                "tap:   sudo ip link set tap0 up\n");
        close(fd);
        return -1;
    }

    /* Non-blocking so the emulator's per-frame poll never stalls Z80
     * execution waiting on the network. */
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    if (out_name && out_name_sz)
        snprintf(out_name, out_name_sz, "%s", ifr.ifr_name);
    return fd;
}

void tap_close(int fd) {
    if (fd >= 0) close(fd);
}

int tap_read(int fd, u8 *buf, size_t maxlen) {
    ssize_t n = read(fd, buf, maxlen);
    if (n > 0) return (int)n;
    if (n == 0) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}

int tap_write(int fd, const u8 *buf, size_t len) {
    ssize_t n = write(fd, buf, len);
    if (n < 0) return -1;
    return (int)n;
}

#else  /* non-Linux: stub everything so the rest of net4cpc.c compiles */

int  tap_open(const char *devname, char *out_name, size_t out_name_sz) {
    (void)devname; (void)out_name; (void)out_name_sz;
    fprintf(stderr, "tap: not supported on this platform\n");
    return -1;
}
void tap_close(int fd) { (void)fd; }
int  tap_read(int fd, u8 *buf, size_t maxlen) {
    (void)fd; (void)buf; (void)maxlen; return -1;
}
int  tap_write(int fd, const u8 *buf, size_t len) {
    (void)fd; (void)buf; (void)len; return -1;
}

#endif

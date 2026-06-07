/* tap.c — Linux TAP device backend. See tap.h. */
#include "tap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#if defined(__linux__)
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
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

/* Whitelist for shell-safe identifiers: alphanumerics, dash, dot, slash,
 * underscore. Used to gate device names and CIDR strings before they
 * end up inside the pkexec sh -c '...' payload. */
static bool tap_str_is_safe(const char *s) {
    if (!s || !*s) return false;
    for (const char *p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '-' || c == '.' || c == '/' || c == '_'))
            return false;
    }
    return true;
}

static bool tap_dev_exists(const char *name) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/net/%s", name);
    return access(path, F_OK) == 0;
}

static const char *tap_owner_name(void) {
    const char *u = getenv("SUDO_USER");
    if (u && *u) return u;
    u = getenv("USER");
    if (u && *u) return u;
    struct passwd *pw = getpwuid(getuid());
    return (pw && pw->pw_name) ? pw->pw_name : "nobody";
}

int tap_auto_create(const char *name, const char *ip_cidr) {
    if (!tap_str_is_safe(name) || !tap_str_is_safe(ip_cidr)) {
        fprintf(stderr, "tap: refusing unsafe device/cidr string\n");
        return -1;
    }
    if (tap_dev_exists(name)) {
        /* Check whether the existing tap's address matches what cfg
         * asks for. If yes, reuse silently. If not, fall through so
         * we recreate with the new address (the iptables rules added
         * earlier are tagged by device name, so deleting + recreating
         * the tap also cleans them up — see tap_auto_destroy). */
        char probe[256];
        snprintf(probe, sizeof(probe),
                 "ip -o addr show %s 2>/dev/null | grep -q '%s'",
                 name, ip_cidr);
        if (system(probe) == 0) {
            fprintf(stderr, "tap: '%s' already configured for %s, reusing\n",
                    name, ip_cidr);
            return 0;
        }
        fprintf(stderr, "tap: '%s' exists with a different address — "
                        "recreating for %s\n", name, ip_cidr);
        tap_auto_destroy(name);
    }
    const char *owner = tap_owner_name();
    if (!tap_str_is_safe(owner)) owner = "root";

    char cmd[2048];
    /* Atomically: create the tap, address it, add it to firewalld
     * trusted, enable v4 forwarding, and add a narrow MASQUERADE +
     * FORWARD pair so 10.0.0.0/24 reaches the wider host network
     * (and DNS upstream via the in-process proxy). Tagged via an
     * iptables comment "1984-<dev>" so destroy can find and undo
     * exactly our rules without disturbing anything else. */
    /* iptables wants the SUBNET (10.0.0.0/24), not the host CIDR
     * (10.0.0.1/24). It normalises the latter to the former silently,
     * so just passing ip_cidr through is safe — and avoids the awk
     * pipeline that previously embedded single quotes inside the
     * pkexec sh -c '...' payload, breaking shell parsing. */
    snprintf(cmd, sizeof(cmd),
        "pkexec sh -c '"
        "set -e; "
        "ip tuntap add dev %s mode tap user %s; "
        "ip link set %s up; "
        "ip addr add %s dev %s; "
        "if command -v firewall-cmd >/dev/null 2>&1; then "
        "  firewall-cmd --zone=trusted --change-interface=%s >/dev/null 2>&1 || true; "
        "fi; "
        "sysctl -w net.ipv4.ip_forward=1 >/dev/null; "
        "iptables -t nat -A POSTROUTING -s %s ! -o %s "
        "  -m comment --comment 1984-%s -j MASQUERADE; "
        "iptables -A FORWARD -i %s -m comment --comment 1984-%s -j ACCEPT; "
        "iptables -A FORWARD -o %s -m comment --comment 1984-%s -j ACCEPT"
        "' 2>&1",
        name, owner, name, ip_cidr, name, name,
        ip_cidr, name, name,
        name, name,
        name, name);

    fprintf(stderr, "tap: auto-creating '%s' (%s) via pkexec — "
                    "a password prompt may appear\n", name, ip_cidr);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "tap: auto-create failed (system() returned %d). "
                        "You can do it manually:\n"
                        "  sudo ip tuntap add dev %s mode tap user %s\n"
                        "  sudo ip link set %s up\n"
                        "  sudo ip addr add %s dev %s\n"
                        "  sudo firewall-cmd --zone=trusted --change-interface=%s\n"
                        "  sudo sysctl -w net.ipv4.ip_forward=1\n"
                        "  sudo iptables -t nat -A POSTROUTING -s 10.0.0.0/24 ! -o %s -j MASQUERADE\n",
                rc, name, owner, name, ip_cidr, name, name, name);
        return -1;
    }
    fprintf(stderr, "tap: '%s' ready — %s, trusted zone, NAT to host network\n",
            name, ip_cidr);
    return 0;
}

void tap_auto_destroy(const char *name) {
    if (!tap_str_is_safe(name)) return;
    if (!tap_dev_exists(name)) return;
    char cmd[1024];
    /* Reverse the auto-create: drop the FORWARD/MASQUERADE rules we
     * tagged on the way up, then delete the tap. The grep|sed dance
     * finds matching rules by our 1984-<dev> comment and deletes them
     * one by one so other emulator instances are untouched. */
    snprintf(cmd, sizeof(cmd),
        "pkexec sh -c '"
        "for tbl in nat filter; do "
        "  while iptables -t \"$tbl\" -S 2>/dev/null | grep -q -- \"1984-%s\"; do "
        "    rule=$(iptables -t \"$tbl\" -S | grep -- \"1984-%s\" | head -1); "
        "    [ -z \"$rule\" ] && break; "
        "    iptables -t \"$tbl\" $(echo \"$rule\" | sed s/^-A/-D/) || break; "
        "  done; "
        "done; "
        "ip tuntap del dev %s mode tap"
        "' >/dev/null 2>&1",
        name, name, name);
    (void)system(cmd);
}

#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)

/* -------------------------------------------------------------------
 * BSD if_tap backend. FreeBSD / NetBSD have a cloning '/dev/tap'
 * device — opening it spawns a tapN and the name is recovered via
 * TAPGIFNAME. OpenBSD doesn't clone; it has fixed '/dev/tap0' …
 * '/dev/tapN' nodes that need to exist (often via 'ifconfig tapN
 * create' beforehand). All three speak raw Ethernet frames (no
 * Linux 4-byte protocol prefix), so tap_read/tap_write are the same.
 *
 * Auto-setup uses a runtime probe for doas → sudo → fail. doas is
 * the default on OpenBSD; FreeBSD/NetBSD users typically have sudo.
 * If neither is present we print the manual command list and let
 * the host-socket fallback take over.
 *
 * NAT/forwarding is intentionally minimal in v1: we bring up the
 * interface and assign an address. Routing outward (pf/ipfw/npf
 * NAT) is documented in NET4CPC.md as a manual follow-up — the
 * three BSDs each have their own packet filter and a "one-click
 * NAT" abstraction across them is more code than this branch should
 * land at once. The CPC is reachable from the host either way.
 * ----------------------------------------------------------------- */

#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <net/if.h>
#if defined(__FreeBSD__) || defined(__NetBSD__)
#  include <net/if_tap.h>            /* TAPGIFNAME */
#endif
#include <ifaddrs.h>

static const char *bsd_elevator(void) {
    /* Runtime probe: prefer doas (lighter, default on OpenBSD),
     * fall back to sudo, give up otherwise. system("command -v X")
     * spawns a shell but only on the first call per process. */
    static const char *picked = NULL;
    static int probed = 0;
    if (!probed) {
        probed = 1;
        if (system("command -v doas >/dev/null 2>&1") == 0) picked = "doas";
        else if (system("command -v sudo >/dev/null 2>&1") == 0) picked = "sudo";
    }
    return picked;
}

int tap_open(const char *devname, char *out_name, size_t out_name_sz) {
    char path[64];
    int fd = -1;

#if defined(__OpenBSD__)
    /* OpenBSD: explicit /dev/tapN node. If devname is "tap5" we open
     * /dev/tap5; if NULL/empty we walk tap0..tap63 until one opens. */
    if (devname && devname[0]) {
        const char *n = devname;
        if (strncmp(n, "tap", 3) == 0) n += 3;
        snprintf(path, sizeof(path), "/dev/tap%s", n);
        fd = open(path, O_RDWR);
        if (fd < 0) {
            fprintf(stderr, "tap: open %s: %s\n", path, strerror(errno));
            return -1;
        }
        if (out_name && out_name_sz)
            snprintf(out_name, out_name_sz, "tap%s", n);
    } else {
        for (int i = 0; i < 64; i++) {
            snprintf(path, sizeof(path), "/dev/tap%d", i);
            fd = open(path, O_RDWR);
            if (fd >= 0) {
                if (out_name && out_name_sz)
                    snprintf(out_name, out_name_sz, "tap%d", i);
                break;
            }
        }
        if (fd < 0) {
            fprintf(stderr,
                "tap: no openable /dev/tapN node found. Create one first:\n"
                "tap:   doas ifconfig tap0 create\n");
            return -1;
        }
    }
#else
    /* FreeBSD / NetBSD: cloning /dev/tap (also accept /dev/tapN for
     * explicit selection). After open() the kernel has bound this fd
     * to a fresh interface; TAPGIFNAME recovers its name. */
    if (devname && devname[0]) {
        const char *n = devname;
        if (strncmp(n, "tap", 3) == 0) n += 3;
        snprintf(path, sizeof(path), "/dev/tap%s", n);
    } else {
        snprintf(path, sizeof(path), "/dev/tap");
    }
    fd = open(path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "tap: open %s: %s\n", path, strerror(errno));
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
#  if defined(TAPGIFNAME)
    if (ioctl(fd, TAPGIFNAME, &ifr) == 0) {
        if (out_name && out_name_sz)
            snprintf(out_name, out_name_sz, "%s", ifr.ifr_name);
    } else
#  endif
    {
        /* Fallback: derive name from devnode minor via fstat. */
        struct stat st;
        if (fstat(fd, &st) == 0) {
            int minor_num = (int)(st.st_rdev & 0xFF);
            if (out_name && out_name_sz)
                snprintf(out_name, out_name_sz, "tap%d", minor_num);
        }
    }
#endif

    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}

void tap_close(int fd) { if (fd >= 0) close(fd); }

int tap_read(int fd, u8 *buf, size_t maxlen) {
    ssize_t n = read(fd, buf, maxlen);
    if (n > 0) return (int)n;
    if (n == 0) return 0;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}

int tap_write(int fd, const u8 *buf, size_t len) {
    ssize_t n = write(fd, buf, len);
    return (n < 0) ? -1 : (int)n;
}

static bool tap_str_is_safe(const char *s) {
    if (!s || !*s) return false;
    for (const char *p = s; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '-' || c == '.' || c == '/' || c == '_'))
            return false;
    }
    return true;
}

static bool tap_dev_exists(const char *name) {
    struct ifaddrs *ifa = NULL, *p;
    if (getifaddrs(&ifa) != 0) return false;
    bool found = false;
    for (p = ifa; p; p = p->ifa_next) {
        if (p->ifa_name && strcmp(p->ifa_name, name) == 0) { found = true; break; }
    }
    freeifaddrs(ifa);
    return found;
}

static const char *bsd_owner_name(void) {
    const char *u = getenv("SUDO_USER");
    if (u && *u) return u;
    u = getenv("DOAS_USER");
    if (u && *u) return u;
    u = getenv("USER");
    if (u && *u) return u;
    struct passwd *pw = getpwuid(getuid());
    return (pw && pw->pw_name) ? pw->pw_name : "nobody";
}

int tap_auto_create(const char *name, const char *ip_cidr) {
    if (!tap_str_is_safe(name) || !tap_str_is_safe(ip_cidr)) {
        fprintf(stderr, "tap: refusing unsafe device/cidr string\n");
        return -1;
    }
    const char *elev = bsd_elevator();
    if (!elev) {
        fprintf(stderr,
            "tap: neither doas nor sudo is installed. Auto-setup needs one\n"
            "tap: of them to run privileged commands. Either install one\n"
            "tap: (`pkg install sudo` on FreeBSD, etc.) or run the manual\n"
            "tap: commands shown after the next failure.\n");
        return -1;
    }
    if (tap_dev_exists(name)) {
        fprintf(stderr, "tap: '%s' already present, reusing (host IP not re-checked on BSD)\n", name);
        return 0;
    }

    /* CIDR like "10.0.0.1/24" → ifconfig wants "10.0.0.1/24" inet on
     * all three BSDs; the form is portable. The 'create' subcommand
     * is also common across them when applied to a tap interface
     * name; if the kernel already auto-created the interface from
     * the device node, 'create' is a no-op (FreeBSD/NetBSD warn but
     * succeed; OpenBSD treats it as creating the if). */
    const char *owner = bsd_owner_name();
    if (!tap_str_is_safe(owner)) owner = "root";
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "%s sh -c '"
        "set -e; "
#if defined(__NetBSD__)
        /* On NetBSD the tap pseudo-device is a loadable kernel module
         * (if_tap) that isn't loaded by default on stock installs.
         * modload returns 0 if already loaded so this is idempotent. */
        "modload if_tap 2>/dev/null || true; "
#endif
        "ifconfig %s create 2>/dev/null || true; "
        "ifconfig %s inet %s up; "
        /* /dev/tapN is root:wheel mode 0600 by default. Chown to the
         * invoking user so the unprivileged 1984 process can open it
         * via tap_open without EACCES. */
        "chown %s /dev/%s; "
        "sysctl -w net.inet.ip.forwarding=1 >/dev/null"
        "' 2>&1",
        elev, name, name, ip_cidr, owner, name);

    fprintf(stderr, "tap: auto-creating '%s' (%s) via %s, "
                    "chown to '%s' — you may be prompted for your password\n",
                    name, ip_cidr, elev, owner);
    int rc = system(cmd);
    if (rc != 0) {
        fprintf(stderr, "tap: auto-create failed (system() returned %d). "
                        "Manual setup:\n"
#if defined(__NetBSD__)
                        "  %s modload if_tap            "
                        "  # NetBSD: load the pseudo-device first\n"
#endif
                        "  %s ifconfig %s create\n"
                        "  %s ifconfig %s inet %s up\n"
                        "  %s chown %s /dev/%s          "
                        "# so 1984 can open the device node\n"
                        "  %s sysctl -w net.inet.ip.forwarding=1\n"
                        "(NAT to host network requires pf/ipfw/npf rules — "
                        "see NET4CPC.md)\n",
                rc,
#if defined(__NetBSD__)
                elev,
#endif
                elev, name, elev, name, ip_cidr,
                elev, owner, name,
                elev);
        return -1;
    }
    fprintf(stderr, "tap: '%s' ready — %s\n"
                    "tap: NAT to wider network not auto-configured on BSD; "
                    "host ↔ CPC works.\n",
                    name, ip_cidr);
    return 0;
}

void tap_auto_destroy(const char *name) {
    if (!tap_str_is_safe(name)) return;
    if (!tap_dev_exists(name)) return;
    const char *elev = bsd_elevator();
    if (!elev) return;
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "%s ifconfig %s destroy >/dev/null 2>&1", elev, name);
    (void)system(cmd);
}

#else  /* non-Linux, non-BSD: stub everything so net4cpc.c compiles */

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
int  tap_auto_create(const char *name, const char *ip_cidr) {
    (void)name; (void)ip_cidr;
    fprintf(stderr, "tap: auto-create not supported on this platform\n");
    return -1;
}
void tap_auto_destroy(const char *name) { (void)name; }

#endif

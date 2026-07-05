#define _GNU_SOURCE
#include "compat_win.h"   /* sockets/fnmatch/statvfs shims; Winsock before windows.h */
#include "m4.h"
#include "leds.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <libgen.h>

int m4_trace = 0;

/* M4 command IDs (from m4cmds.i) */
#define C_OPEN        0x4301
#define C_READ        0x4302
#define C_WRITE       0x4303
#define C_CLOSE       0x4304
#define C_SEEK        0x4305
#define C_READDIR     0x4306
#define C_EOF         0x4307
#define C_CD          0x4308
#define C_FREE        0x4309
#define C_FTELL       0x430A
#define C_ERASEFILE   0x430E
#define C_RENAME      0x430F
#define C_MAKEDIR     0x4310
#define C_FSIZE       0x4311
#define C_READ2       0x4312
#define C_GETPATH     0x4313
#define C_ROMSOFF     0x4318
#define C_NMIOFF      0x4319
#define C_RAMDISOFF   0x431A
#define C_WRITE2      0x431B
#define C_HTTPGET     0x4320
#define C_SETNETWORK  0x4321
#define C_M4OFF       0x4322
#define C_NETSTAT     0x4323
#define C_TIME        0x4324
#define C_DIRSETARGS  0x4325
#define C_VERSION     0x4326
#define C_HTTPGETMEM  0x4328
#define C_COPYFILE    0x432A
#define C_ROMLIST     0x432C
#define C_DSKEXT      0x4330
#define C_SDREAD      0x4314
#define C_SDWRITE     0x4315
#define C_NETSOCKET   0x4331
#define C_NETCONNECT  0x4332
#define C_NETCLOSE    0x4333
#define C_NETSEND     0x4334
#define C_NETRECV     0x4335
#define C_NETHOSTIP   0x4336
#define C_NETRSSI     0x4337
#define C_ROMLOW      0x433D
#define C_NETBIND     0x4338
#define C_NETLISTEN   0x4339
#define C_NETACCEPT   0x433A
#define C_GETNETWORK  0x433B
#define C_WIFIPOW     0x433C
#define C_CONFIG      0x43FE
/* 1984-only: drain N bytes from arbitrary M4 memory. Used by symbnet's
 * daemon (netd-1984.exe) to poll sock_info between commands without a
 * bus-mapped buffer. Args: addr-lo, addr-hi, length. */
#define C_READMEM     0x43FD

/* Response base address — virtual; data is written to m->bus_mem, not CPC RAM,
 * to avoid corrupting screen memory (CPC screen RAM lives at 0xC000-0xFFFF). */
#define RESP_BASE 0xE800u

/* ---- Response helpers (write to M4 board's own buffer, not CPC RAM) ---- */

static void resp_u8(M4 *m, u16 *off, u8 v) {
    if (*off < sizeof(m->bus_mem))
        m->bus_mem[(*off)] = v;
    (*off)++;
}
static void resp_u16le(M4 *m, u16 *off, u16 v) {
    resp_u8(m, off, v & 0xFF);
    resp_u8(m, off, v >> 8);
}
static void resp_u32le(M4 *m, u16 *off, u32 v) {
    resp_u8(m, off, v & 0xFF);
    resp_u8(m, off, (v >> 8) & 0xFF);
    resp_u8(m, off, (v >> 16) & 0xFF);
    resp_u8(m, off, v >> 24);
}
static void resp_str(M4 *m, u16 *off, const char *s) {
    while (*s) resp_u8(m, off, (u8)*s++);
    resp_u8(m, off, 0);
}

/* Responses use the same framing as commands: byte 0 is the number of
 * following bytes, then the echoed command word and command-specific data. */
static void resp_frame(M4 *m, u16 cmd, u16 end) {
    m->bus_mem[0] = (u8)(end - 1);
    m->bus_mem[1] = (u8)(cmd & 0xFF);
    m->bus_mem[2] = (u8)(cmd >> 8);
}

/* ---- Path helpers ---- */

/* Build a host filesystem path from an M4 CPC path (absolute or relative to cwd).
 * Ensures the resolved path stays within m->root. Returns true on success. */
/* Walk a relative path one segment at a time starting from `base_dir`,
 * matching each segment case-insensitively against entries in that
 * directory (FAT filesystems are case-insensitive; the host isn't).
 * Writes the resolved absolute host path into `out`. Returns true if
 * the full path matched a real filesystem entry. */
static bool walk_path_nocase(const char *base_dir, const char *rel_path,
                             char *out, size_t outsz) {
    char cur[PATH_MAX];
    snprintf(cur, sizeof(cur), "%s", base_dir);

    const char *p = rel_path;
    while (*p == '/') p++;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t seglen = slash ? (size_t)(slash - p) : strlen(p);
        if (seglen == 0) { p += 1; continue; }
        char seg[256];
        if (seglen >= sizeof(seg)) return false;
        memcpy(seg, p, seglen); seg[seglen] = '\0';

        DIR *d = opendir(cur);
        if (!d) return false;
        char matched[256] = "";
        struct dirent *de;
        while ((de = readdir(d))) {
            if (strcasecmp(de->d_name, seg) == 0) {
                snprintf(matched, sizeof(matched), "%s", de->d_name);
                break;
            }
        }
        closedir(d);
        if (!matched[0]) return false;
        size_t curlen = strlen(cur);
        snprintf(cur + curlen, sizeof(cur) - curlen, "/%s", matched);

        p += seglen;
        while (*p == '/') p++;
    }
    snprintf(out, outsz, "%s", cur);
    return true;
}

static bool resolve_path(const M4 *m, const char *cpc_path, char *out, size_t outsz) {
    if (!m->root[0]) return false;

    /* Replace backslashes with forward slashes */
    char clean[M4_PATH_MAX];
    size_t ci = 0;
    for (size_t i = 0; cpc_path[i] && ci < sizeof(clean) - 1; i++)
        clean[ci++] = (cpc_path[i] == '\\') ? '/' : cpc_path[i];
    clean[ci] = '\0';

    /* Build the absolute path the caller refers to (rooted at SD root or cwd) */
    char rel[M4_PATH_MAX * 2];
    if (clean[0] == '/') {
        snprintf(rel, sizeof(rel), "%s", clean + 1);
    } else {
        const char *cwd = m->cwd[0] == '/' ? m->cwd + 1 : m->cwd;
        if (*cwd)
            snprintf(rel, sizeof(rel), "%s/%s", cwd, clean);
        else
            snprintf(rel, sizeof(rel), "%s", clean);
    }

    /* Resolve case-insensitively (FAT filesystem semantics on a case-sensitive
     * host). If the leaf doesn't exist (e.g. SAVE creating a new file), fall
     * back to resolving the parent and appending the original leaf name. */
    if (walk_path_nocase(m->root, rel, out, outsz))
        return true;

    char relcopy[M4_PATH_MAX * 2];
    snprintf(relcopy, sizeof(relcopy), "%s", rel);
    char *last = strrchr(relcopy, '/');
    char parent[PATH_MAX];
    const char *leaf;
    if (last) {
        *last = '\0';
        leaf = last + 1;
        if (!walk_path_nocase(m->root, relcopy, parent, sizeof(parent)))
            return false;
    } else {
        snprintf(parent, sizeof(parent), "%s", m->root);
        leaf = relcopy;
    }
    snprintf(out, outsz, "%s/%s", parent, leaf);
    /* Security: must stay within root */
    size_t rootlen = strlen(m->root);
    return strncmp(out, m->root, rootlen) == 0;
}

/* ---- File descriptor helpers ---- */

static int alloc_fd(M4 *m) {
    for (int i = 0; i < M4_MAX_FDS; i++)
        if (!m->fds[i].in_use) return i + 1;
    return -1;
}

static bool valid_fd(const M4 *m, u8 fd) {
    return fd >= 1 && fd <= M4_MAX_FDS && m->fds[fd - 1].in_use;
}

/* ---- Public API ---- */

/* True when the file API should route through the FAT image (image mode):
 * no host directory configured AND the image mounts as a valid FAT volume. */
static bool m4_use_fat(const M4 *m) {
    return !m->root[0] && m->image_mounted;
}

/* Build an absolute FAT path from a caller-supplied CPC path (which may be
 * relative or absolute) and the current cwd. */
static void fat_abs_path(const M4 *m, const char *cpc_path,
                         char *out, size_t outsz) {
    /* Normalise backslashes to forward slashes */
    char clean[M4_PATH_MAX];
    size_t ci = 0;
    for (size_t i = 0; cpc_path[i] && ci < sizeof(clean) - 1; i++)
        clean[ci++] = (cpc_path[i] == '\\') ? '/' : cpc_path[i];
    clean[ci] = '\0';

    if (clean[0] == '/') {
        snprintf(out, outsz, "%s", clean);
    } else {
        const char *cwd = m->cwd[0] ? m->cwd : "/";
        if (strcmp(cwd, "/") == 0)
            snprintf(out, outsz, "/%s", clean);
        else
            snprintf(out, outsz, "%s/%s", cwd, clean);
    }
}

/* Open the raw disk image file if image_path is set. Also try mounting it as
 * a FAT16/FAT32 volume so the file API can route through it when no host
 * directory is configured. */
static void m4_open_image(M4 *m) {
    if (m->image_mounted) { fat_unmount(&m->image_vol); m->image_mounted = false; }
    if (m->image_fp) { fclose(m->image_fp); m->image_fp = NULL; }
    m->image_read_only = false;
    if (!m->image_path[0]) return;
    struct stat st;
    if (stat(m->image_path, &st) != 0 || !S_ISREG(st.st_mode)) return;
    m->image_fp = fopen(m->image_path, "r+b");
    if (!m->image_fp) {
        m->image_fp = fopen(m->image_path, "rb");
        m->image_read_only = m->image_fp != NULL;
    }
    if (m->image_fp)
        m->image_mounted = fat_mount(&m->image_vol, m->image_fp);
}

/* ---- Network helpers ---- */

/* sock_info layout (16 bytes per socket): status, lastcmd, rxlo, rxhi,
 * ip0..ip3, portlo, porthi, 6 reserved. Mirror M4Socket → sock_mem. */
static void sync_sock_mem(M4 *m, int s) {
    if (s < 0 || s >= M4_NSOCKS) return;
    u8 *p = &m->sock_mem[s * 16];
    p[0]  = m->sockets[s].status;
    p[1]  = m->sockets[s].lastcmd;
    p[2]  = (u8)(m->sockets[s].rx_count & 0xFF);
    p[3]  = (u8)(m->sockets[s].rx_count >> 8);
    memcpy(&p[4], m->sockets[s].peer_ip, 4);
    p[8]  = (u8)(m->sockets[s].peer_port & 0xFF);
    p[9]  = (u8)(m->sockets[s].peer_port >> 8);

    /* SymbOS netd-m4c.exe workaround: when only one TCP socket is open,
     * mirror its 16-byte sock_info into every other slot (5..15) so the
     * daemon's wrong-slot reads (caused by m4csct returning garbage A in
     * 0..15) still see correct status/rx_count and keep polling. */
    int active = -1;
    for (int i = 1; i < M4_NSOCKS; i++) {
        if (m->sockets[i].fd < 0) continue;
        if (active >= 0) { active = -1; break; }  /* >1 open: don't broadcast */
        active = i;
    }
    if (active >= 0) {
        u8 *src = &m->sock_mem[active * 16];
        for (int slot = 0; slot < 16; slot++) {
            if (slot == active) continue;
            memcpy(&m->sock_mem[slot * 16], src, 16);
        }
    }
}

static void net_close_socket(M4 *m, int s) {
    if (s < 0 || s >= M4_NSOCKS) return;
    if (m->sockets[s].fd >= 0) close(m->sockets[s].fd);
    memset(&m->sockets[s], 0, sizeof(m->sockets[s]));
    m->sockets[s].fd     = -1;
    m->sockets[s].status = 0;
    sync_sock_mem(m, s);
}

static u16 telnet_filter_inplace(u8 *buf, u16 len) {
    u16 r = 0;
    u16 w = 0;
    while (r < len) {
        u8 c = buf[r++];
        if (c != 0xFF) {
            buf[w++] = c;
            continue;
        }
        if (r >= len) break;

        u8 cmd = buf[r++];
        if (cmd == 0xFF) {
            buf[w++] = 0xFF;
            continue;
        }

        /* Skip a simple IAC command triplet: IAC WILL/WONT/DO/DONT option. */
        if (cmd == 0xFB || cmd == 0xFC || cmd == 0xFD || cmd == 0xFE) {
            if (r < len) r++;
            continue;
        }

        /* Skip subnegotiation blocks: IAC SB ... IAC SE. */
        if (cmd == 0xFA) {
            while (r + 1 < len) {
                if (buf[r] == 0xFF && buf[r + 1] == 0xF0) {
                    r += 2;
                    break;
                }
                r++;
            }
            continue;
        }

        /* Other IAC commands are control-only; drop them. */
    }
    return w;
}

/* Probe an in-flight connect on socket s; updates status to connected (0) or
 * error (240+) when the kernel has decided. */
static void net_poll_socket(M4 *m, int s) {
    if (s < 0 || s >= M4_NSOCKS) return;
    M4Socket *sk = &m->sockets[s];
    if (sk->fd < 0) return;
    if (sk->connecting) {
        struct pollfd pfd = { .fd = sk->fd, .events = POLLOUT };
        int r = poll(&pfd, 1, 0);
        if (r > 0 && (pfd.revents & (POLLOUT | POLLERR | POLLHUP))) {
            int soerr = 0; socklen_t l = sizeof(soerr);
            getsockopt(sk->fd, SOL_SOCKET, SO_ERROR, (char *)&soerr, &l);
            sk->connecting = false;
            sk->status = soerr ? 240 : 0;  /* 0 = connected/idle, 240+ = error */
        }
    }
    /* Peek RX bytes available */
    if (sk->status == 0 || sk->status == 5) {
        int avail = 0;
        if (sock_fionread(sk->fd, &avail) == 0) {
            if (avail < 0) avail = 0;
            if (avail > 0xFFFF) avail = 0xFFFF;
            sk->rx_count = (u16)avail;
            /* Detect remote close: read 0 bytes available but socket dead */
            if (avail == 0) {
                char b;
                ssize_t n = recv(sk->fd, &b, 1, MSG_PEEK | MSG_DONTWAIT);
                if (n == 0) sk->status = 3; /* remote closed */
            }
        }
    }
    sync_sock_mem(m, s);
}

void m4_init(M4 *m, const char *root) {
    memset(m, 0, sizeof(*m));
    m->nmi_enabled = false;  /* ROM enables NMI explicitly via C_NMION after init */
    snprintf(m->cwd, sizeof(m->cwd), "/");
    if (root && root[0])
        snprintf(m->root, sizeof(m->root), "%s", root);
    snprintf(m->dir_filter, sizeof(m->dir_filter), "*");
    for (int i = 0; i < M4_NSOCKS; i++) m->sockets[i].fd = -1;
}

void m4_set_image(M4 *m, const char *image_path) {
    if (image_path && image_path[0])
        snprintf(m->image_path, sizeof(m->image_path), "%s", image_path);
    else
        m->image_path[0] = '\0';
    m4_open_image(m);
}

void m4_tick(M4 *m) {
    /* Polled from cpc_frame so the sock_info bytes (visible to CPC code via
     * the bus bypass at 0xFE00) stay current while applications busy-loop on
     * the status byte without sending any M4 commands. */
    for (int i = 0; i < M4_NSOCKS; i++)
        if (m->sockets[i].fd >= 0) net_poll_socket(m, i);
}

void m4_reset(M4 *m) {
    /* Close all open files and directories, reset command buffer and cwd */
    for (int i = 0; i < M4_MAX_FDS; i++) {
        if (m->fds[i].fp)   fclose(m->fds[i].fp);
        if (m->fds[i].fatf) fat_close(m->fds[i].fatf);
        m->fds[i].in_use = false;
        m->fds[i].fp     = NULL;
        m->fds[i].fatf   = NULL;
    }
    if (m->dir_dp)  { closedir(m->dir_dp); m->dir_dp = NULL; }
    if (m->dir_fat) { fat_closedir(m->dir_fat); m->dir_fat = NULL; }
    m->cmd_len = 0;
    snprintf(m->cwd, sizeof(m->cwd), "/");
    snprintf(m->dir_filter, sizeof(m->dir_filter), "*");
    /* Re-open image fd (if image_path is set) */
    m4_open_image(m);
    /* Tear down any open TCP sockets and clear sock_info */
    for (int i = 0; i < M4_NSOCKS; i++) net_close_socket(m, i);
    m->nmi_enabled = false;
    m->last_error = M4_OK;
}

void m4_dataport_write(M4 *m, u8 val) {
    if (m->cmd_len < M4_CMD_BUF)
        m->cmd_buf[m->cmd_len++] = val;
}

u8 m4_dataport_read(M4 *m) {
    (void)m;
    return 0x00; /* always ready */
}

/* ---- Command dispatch ---- */

/* "1984 compatibility shim": patch the M4ROM helper table to point at
 * trap stubs we install in our own bus_mem. The daemon's banked m4crcv
 * does a `JP <helper_recv_addr>` after selecting upper-ROM slot 0 — on
 * real M4 hardware the board makes its own ROM code visible at that
 * address regardless of slot. We can't replicate that without breaking
 * SymbOS screen-RAM reads, so instead we route the JP into a spot we
 * fully control: addresses inside bus_mem, which our bus bypass already
 * serves whenever M4 is set up. The stub we install there is just a
 * tiny `LD BC, 0xFD3F; OUT (C), A` — the emulator catches that port
 * write and runs the bulk copy in C, then sets PC to IX (the return
 * address the daemon passed) and continues. */
#define M4_HSEND_TRAP_ADDR 0xF300u
#define M4_HRECV_TRAP_ADDR 0xF310u

void m4_install_helper_shim(M4 *m, Mem *mem) {
    if (!mem || !mem->rom_ext_present[M4_ROM_SLOT]) return;
    u8 *rom = mem->rom_ext[M4_ROM_SLOT];
    /* M4ROM helper-pointer table lives at 0xE430 (file offset 0x2430).
     * Layout: helper_send (LE word), helper_recv (LE word). Daemon's
     * m4crom copies 4 bytes from here into its own m4cromhsn/m4cromhrc. */
    rom[0x2430] = M4_HSEND_TRAP_ADDR & 0xFF;
    rom[0x2431] = M4_HSEND_TRAP_ADDR >> 8;
    rom[0x2432] = M4_HRECV_TRAP_ADDR & 0xFF;
    rom[0x2433] = M4_HRECV_TRAP_ADDR >> 8;

    /* Stub at the trap address: LD BC, 0xFD3{E,F} ; OUT (C), A.
     * After the OUT, the trap handler sets PC = IX, so we don't
     * include any return instruction here. */
    static const u8 hsend_stub[] = { 0x01, 0x3E, 0xFD, 0xED, 0x79 };
    static const u8 hrecv_stub[] = { 0x01, 0x3F, 0xFD, 0xED, 0x79 };
    memcpy(&m->bus_mem[M4_HSEND_TRAP_ADDR - 0xE800u], hsend_stub, sizeof(hsend_stub));
    memcpy(&m->bus_mem[M4_HRECV_TRAP_ADDR - 0xE800u], hrecv_stub, sizeof(hrecv_stub));
}

bool m4_ackport_write(M4 *m, Mem *mem) {
    /* Install the helper-shim on first strobe (idempotent — safe to do
     * unconditionally, but the trap stub addresses never move). */
    m4_install_helper_shim(m, mem);

    /* Refresh sock_info for any sockets that were busy connecting or have
     * data waiting — the daemon polls sock_info between commands. */
    for (int i = 0; i < M4_NSOCKS; i++)
        if (m->sockets[i].fd >= 0) net_poll_socket(m, i);

    if (m->cmd_len < 3) {
        m->last_error = M4_ERR_IO;
        resp_frame(m, 0, 3);
        m->cmd_len = 0;
        return m->nmi_enabled;
    }

    /* Find the packet header. Normal case: cmd_buf[0] is the header-byte
     * count (per daemon's m4ccmd0) and cmd_buf[2] holds the command's
     * high byte (always 0x43 for M4 ops). Fall back to a scan only if
     * offset 0 doesn't look like a valid header — this handles the case
     * where unrelated bus writes to the broadly-decoded 0xFExx port range
     * land in cmd_buf before a legitimate M4 command. We CAN'T validate
     * total packet length against cmd_buf[0] (variable-payload commands
     * like C_NETSEND have payload bytes written after the header via
     * m4csnd before the ACK strobe), so we trust the 0x43 sentinel. */
    int packet_start = 0;
    if (m->cmd_len < 3 || m->cmd_buf[2] != 0x43) {
        packet_start = -1;
        for (int i = 1; i <= m->cmd_len - 3; i++) {
            if (m->cmd_buf[i + 2] == 0x43) { packet_start = i; break; }
        }
        if (packet_start < 0) {
            m->last_error = M4_ERR_IO;
            resp_frame(m, 0, 3);
            m->cmd_len = 0;
            return m->nmi_enabled;
        }
    }

    const u8 *packet = &m->cmd_buf[packet_start];
    int packet_len = m->cmd_len - packet_start;
    u16 cmd = (u16)packet[1] | ((u16)packet[2] << 8);

    m->ram_mode = false;
    m->ram_mode_reads = 0;
    const u8 *p = packet + 3;       /* params */
    int plen    = packet_len - 3;
    u8  err     = M4_ERR_NOTSUP;
    u16 roff    = 3;                 /* response data written starting at RESP_BASE+3 (M4 protocol) */

    if (m4_trace) {
        extern int cpc_frame_count;
        fprintf(stderr, "[m4 f%d] CMD %04X (%d args):", cpc_frame_count, cmd, plen);
        for (int i = 0; i < plen && i < 16; i++) fprintf(stderr, " %02X", p[i]);
        if (plen > 16) fprintf(stderr, " ...(+%d)", plen - 16);
        fprintf(stderr, "\n");
    }

    switch (cmd) {

    /* ---- System ---- */

    case C_CONFIG: {
        /* Generic config area write. Packet: [offset][data...].
         * Writes payload bytes into the M4 board's own config buffer
         * (read via the bus bypass at 0xF400-0xF500). */
        if (plen >= 1) {
            int base = p[0];
            int n = plen - 1;
            for (int i = 0; i < n && (base + i) < (int)sizeof(m->cfg_mem); i++)
                m->cfg_mem[base + i] = p[1 + i];
            if (p[0] == 5 && plen >= 2)
                m->init_count = p[1];
        }
        err = M4_OK;
        break;
    }

    case C_VERSION:
        err = M4_OK;
        resp_str(m, &roff, "M4 1984 Emulator");
        break;

    case C_TIME: {
        /* M4 protocol: returns the current date/time as a null-terminated
         * ASCII string in the form "hh:mm:ss yyyy-mm-dd". */
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        char buf[32];
        if (tm)
            strftime(buf, sizeof(buf), "%H:%M:%S %Y-%m-%d", tm);
        else
            snprintf(buf, sizeof(buf), "00:00:00 1980-01-01");
        err = M4_OK;
        resp_str(m, &roff, buf);
        break;
    }

    case C_NMIOFF:
        m->nmi_enabled = (plen > 0) ? (p[0] == 0) : false;
        if (m4_trace) fprintf(stderr, "[m4]      NMI -> %s\n",
                              m->nmi_enabled ? "ENABLED" : "DISABLED");
        err = M4_OK;
        break;

    case C_ROMLOW:
        /* M4 firmware v2.0.7+ board-detection helper used by FUZIX.
         * arg = 2 → "hack" mode: route subsequent lower-ROM reads to
         *           the M4 snapshot-ROM image instead of OS_6128.ROM.
         *           FUZIX reads byte 0x100 expecting 'M' ("MV - SNA"
         *           header) and byte 0x0 expecting the M4 rom number.
         * arg = 1 → restore default lower ROM.
         * We back this with a stub (mem->m4_snapshot_rom_stub) that
         * has just the two bytes FUZIX checks; enough to pass
         * detection. */
        if (plen > 0) {
            if (p[0] == 2) mem->lower_rom_override = mem->m4_snapshot_rom_stub;
            else if (p[0] == 1) mem->lower_rom_override = NULL;
        }
        if (m4_trace) fprintf(stderr, "[m4]      ROMLOW arg=%u -> override %s\n",
                              plen ? p[0] : 0,
                              mem->lower_rom_override ? "STUB" : "OFF");
        err = M4_OK;
        break;

    case C_M4OFF:
    case C_ROMSOFF:
    case C_RAMDISOFF:
        err = M4_OK;
        break;

    /* ---- Filesystem navigation ---- */

    case C_GETPATH:
        /* The cwd is meaningful in both directory and image modes, so always
         * return it. If we returned an error in image-only mode, M4ROM's
         * disp_msg would dump whatever stale bytes happen to be in the
         * response buffer to the screen — potentially mid-string control
         * codes that flip the screen mode (which is exactly how the
         * "cat switches to mode 2" bug used to manifest). */
        err = M4_OK;
        resp_str(m, &roff, m->cwd[0] ? m->cwd : "/");
        break;

    case C_CD: {
        if (plen < 1 || (!m->root[0] && !m4_use_fat(m))) { err = M4_ERR_IO; break; }
        const char *path = (const char *)p;
        if (strcmp(path, "/") == 0 || strcmp(path, "\\") == 0) {
            snprintf(m->cwd, sizeof(m->cwd), "/");
            err = M4_OK;
            break;
        }
        if (strcmp(path, "..") == 0) {
            char *last = strrchr(m->cwd, '/');
            if (last && last != m->cwd) *last = '\0';
            else snprintf(m->cwd, sizeof(m->cwd), "/");
            err = M4_OK;
            break;
        }
        if (m4_use_fat(m)) {
            char abs[M4_PATH_MAX];
            fat_abs_path(m, path, abs, sizeof(abs));
            if (!fat_dir_exists(&m->image_vol, abs)) { err = M4_ERR_NOFILE; break; }
            snprintf(m->cwd, sizeof(m->cwd), "%s", abs);
            err = M4_OK;
            break;
        }
        char hostpath[M4_PATH_MAX];
        if (!resolve_path(m, path, hostpath, sizeof(hostpath))) { err = M4_ERR_NOFILE; break; }
        struct stat st;
        if (stat(hostpath, &st) != 0 || !S_ISDIR(st.st_mode)) { err = M4_ERR_NOFILE; break; }
        const char *rel = hostpath + strlen(m->root);
        snprintf(m->cwd, sizeof(m->cwd), "%s", rel[0] ? rel : "/");
        err = M4_OK;
        break;
    }

    case C_FREE: {
        u32 free_kb = 0;
        if (m4_use_fat(m)) {
            free_kb = fat_free_kb(&m->image_vol);
        } else if (m->root[0]) {
            unsigned long long fb = compat_fs_free_bytes(m->root);
            if (fb) free_kb = (u32)(fb / 1024);
        }
        char buf[40];
        snprintf(buf, sizeof(buf), "\r\n%uK free\r\n\r\n", (unsigned)free_kb);
        err = M4_OK;
        resp_str(m, &roff, buf);
        break;
    }

    /* ---- Directory listing ---- */

    case C_DIRSETARGS: {
        if (!m->root[0] && !m4_use_fat(m)) { err = M4_ERR_IO; break; }
        if (m->dir_dp)  { closedir(m->dir_dp); m->dir_dp = NULL; }
        if (m->dir_fat) { fat_closedir(m->dir_fat); m->dir_fat = NULL; }

        if (plen > 0 && p[0])
            snprintf(m->dir_filter, sizeof(m->dir_filter), "%s", (const char *)p);
        else
            snprintf(m->dir_filter, sizeof(m->dir_filter), "*");

        if (m4_use_fat(m)) {
            m->dir_fat = fat_opendir(&m->image_vol, m->cwd[0] ? m->cwd : "/");
            err = m->dir_fat ? M4_OK : M4_ERR_IO;
        } else {
            char hostpath[M4_PATH_MAX];
            snprintf(hostpath, sizeof(hostpath), "%s%s", m->root, m->cwd);
            m->dir_dp = opendir(hostpath);
            err = m->dir_dp ? M4_OK : M4_ERR_IO;
        }
        break;
    }

    case C_READDIR: {
        /* M4ROM's catalog loop checks rom_response+0 == 2 for EOF. */
        if (!m->dir_dp && !m->dir_fat) { err = 2; break; }

        char  entry_name[256];
        bool  is_dir = false;
        u32   fsize  = 0;

        if (m->dir_fat) {
            for (;;) {
                if (!fat_readdir(m->dir_fat, entry_name, sizeof(entry_name),
                                 &fsize, &is_dir)) { err = 2; goto readdir_done; }
                if (fnmatch(m->dir_filter, entry_name,
                            FNM_NOESCAPE | FNM_CASEFOLD) == 0)
                    break;
            }
            if (is_dir) fsize = 0;
        } else {
            struct dirent *de = NULL;
            for (;;) {
                de = readdir(m->dir_dp);
                if (!de) { err = 2; goto readdir_done; }
                if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                    continue;
                if (fnmatch(m->dir_filter, de->d_name,
                            FNM_NOESCAPE | FNM_CASEFOLD) != 0)
                    continue;
                break;
            }
            snprintf(entry_name, sizeof(entry_name), "%s", de->d_name);

            char hostpath[M4_PATH_MAX];
            snprintf(hostpath, sizeof(hostpath), "%s%s/%s",
                     m->root, m->cwd, entry_name);
            struct stat st;
            if (stat(hostpath, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
                fsize  = is_dir ? 0 : (u32)st.st_size;
            }
        }

        /* Format as 8.3 for display.
         * Layout consumed by M4ROM's direntry_workbuf:
         *   bytes 0-7:   name (space-padded, uppercase)
         *   byte  8:     '.' (dot — even for directories)
         *   bytes 9-11:  extension (space-padded, uppercase)
         *   bytes 12-16: 5 chars ASCII size (right-aligned, e.g. "    6")
         *   byte  17:    '\0' terminator
         *   bytes 18-19: binary file size (little-endian, low 16 bits)
         * Directories are prefixed with '>' per M4 board convention. */
        char name8[9] = "        ";
        char ext3[4]  = "   ";
        const char *src = entry_name;
        if (is_dir) {
            name8[0] = '>';
            for (int i = 1; i < 8 && src[i-1]; i++)
                name8[i] = (char)toupper((unsigned char)src[i-1]);
        } else {
            const char *dot = strrchr(src, '.');
            int nlen = dot ? (int)(dot - src) : (int)strlen(src);
            if (nlen > 8) nlen = 8;
            for (int i = 0; i < nlen; i++)
                name8[i] = (char)toupper((unsigned char)src[i]);
            if (dot) {
                const char *e = dot + 1;
                for (int i = 0; i < 3 && e[i]; i++)
                    ext3[i] = (char)toupper((unsigned char)e[i]);
            }
        }

        err = M4_OK;
        for (int i = 0; i < 8; i++) resp_u8(m, &roff, (u8)name8[i]);
        resp_u8(m, &roff, '.');
        for (int i = 0; i < 3; i++) resp_u8(m, &roff, (u8)ext3[i]);
        /* 5-char right-aligned size (or "  DIR" for directories) */
        char szbuf[6];
        if (is_dir) {
            snprintf(szbuf, sizeof(szbuf), "  DIR");
        } else {
            snprintf(szbuf, sizeof(szbuf), "%5u",
                     (unsigned)(fsize > 99999 ? 99999 : fsize));
        }
        for (int i = 0; i < 5; i++) resp_u8(m, &roff, (u8)szbuf[i]);
        resp_u8(m, &roff, 0x00);                      /* terminator */
        resp_u16le(m, &roff, (u16)(fsize & 0xFFFF));  /* binary size */
        readdir_done:;
        break;
    }

    /* ---- File I/O ---- */

    case C_OPEN: {
        /* M4 board protocol:
         *   mode without FA_REALMODE (0x80): AMSDOS-compat — always fd=1 for
         *     read (FA_READ=0x01), fd=2 for write (FA_WRITE=0x02). M4ROM hardcodes
         *     these fds (e.g. _cas_in_open's fread uses fd=1 literally).
         *   mode with FA_REALMODE: dynamic fd from a pool (fds 3..M4_MAX_FDS).
         * Response: resp+3 = fd, resp+4 = 0 on success, non-zero error otherwise. */
        u8 open_fd = 0xFF, open_err = M4_ERR_IO;
        if ((m->root[0] || m4_use_fat(m)) && plen >= 2) {
            u8 mode = p[0];
            const char *name = (const char *)&p[1];
            bool is_write = (mode & 0x02) != 0;
            bool create_always = (mode & 0x08) != 0;
            bool open_always = (mode & 0x10) != 0;

            if (m4_use_fat(m)) {
                /* Image-mode: route through the FAT volume. */
                char abs[M4_PATH_MAX];
                fat_abs_path(m, name, abs, sizeof(abs));
                FatFile *ff = NULL;
                if (is_write && open_always && !create_always) {
                    ff = fat_open(&m->image_vol, abs, false);
                    if (ff) {
                        ff->write_mode = true;
                    } else {
                        ff = fat_open(&m->image_vol, abs, true);
                    }
                } else {
                    ff = fat_open(&m->image_vol, abs, is_write);
                }
                if (!ff && !is_write && !strchr(name, '.')) {
                    /* Try .BAS / .BIN auto-extension for read mode. */
                    static const char *exts[] = { ".BAS", ".BIN", NULL };
                    for (int e = 0; exts[e] && !ff; e++) {
                        char tryabs[M4_PATH_MAX];
                        snprintf(tryabs, sizeof(tryabs), "%s%s", abs, exts[e]);
                        ff = fat_open(&m->image_vol, tryabs, false);
                    }
                }
                int idx;
                if (mode & 0x80) {
                    idx = -1;
                    for (int i = 2; i < M4_MAX_FDS; i++)
                        if (!m->fds[i].in_use) { idx = i + 1; break; }
                } else {
                    idx = (mode & 0x02) ? 2 : 1;
                    if (m->fds[idx - 1].in_use) {
                        if (m->fds[idx - 1].fp)   fclose(m->fds[idx - 1].fp);
                        if (m->fds[idx - 1].fatf) fat_close(m->fds[idx - 1].fatf);
                        m->fds[idx - 1].fp = NULL;
                        m->fds[idx - 1].fatf = NULL;
                        m->fds[idx - 1].in_use = false;
                    }
                }
                if (!ff) {
                    open_err = M4_ERR_NOFILE;
                } else if (idx < 0) {
                    fat_close(ff);
                    open_err = M4_ERR_FULL;
                } else {
                    m->fds[idx - 1].fatf   = ff;
                    m->fds[idx - 1].in_use = true;
                    open_fd  = (u8)idx;
                    open_err = M4_OK;
                }
                err = open_err;
                resp_u8(m, &roff, open_fd);
                resp_u8(m, &roff, open_err);
                break;
            }

            char hostpath[M4_PATH_MAX];
            bool path_ok = resolve_path(m, name, hostpath, sizeof(hostpath));
            if (path_ok && !is_write) {
                struct stat st;
                /* Must exist as a regular file (not a directory) for read mode */
                if (stat(hostpath, &st) != 0 || !S_ISREG(st.st_mode))
                    path_ok = false;
            }
            /* M4 board auto-extension (per spinpoint.org/cpc/m4info): for READ
             * mode only, if the verbatim name isn't found, retry with .BAS then
             * .BIN appended. Skip when caller already gave an extension. */
            if (!path_ok && !is_write && !strchr(name, '.')) {
                char tryname[M4_PATH_MAX];
                static const char *exts[] = { ".BAS", ".BIN", NULL };
                for (int e = 0; exts[e] && !path_ok; e++) {
                    snprintf(tryname, sizeof(tryname), "%s%s", name, exts[e]);
                    if (resolve_path(m, tryname, hostpath, sizeof(hostpath))) {
                        struct stat st;
                        if (stat(hostpath, &st) == 0 && S_ISREG(st.st_mode))
                            path_ok = true;
                    }
                }
            }
            if (!path_ok) {
                open_err = M4_ERR_NOFILE;
            } else {
                int idx;
                if (mode & 0x80) {                  /* real-mode dynamic alloc (3..N) */
                    idx = -1;
                    for (int i = 2; i < M4_MAX_FDS; i++)
                        if (!m->fds[i].in_use) { idx = i + 1; break; }
                } else {                            /* AMSDOS-compat fixed fd */
                    idx = (mode & 0x02) ? 2 : 1;
                    /* Force-close any stale fd left over from a prior session */
                    if (m->fds[idx - 1].in_use && m->fds[idx - 1].fp) {
                        fclose(m->fds[idx - 1].fp);
                        m->fds[idx - 1].fp = NULL;
                        m->fds[idx - 1].in_use = false;
                    }
                }
                if (idx < 0) {
                    open_err = M4_ERR_FULL;
                } else {
                    FILE *fp = NULL;
                    if (is_write && open_always && !create_always) {
                        fp = fopen(hostpath, "r+b");
                        if (!fp) fp = fopen(hostpath, "w+b");
                    } else {
                        fp = fopen(hostpath, is_write ? "w+b" : "rb");
                    }
                    if (!fp) {
                        open_err = M4_ERR_NOFILE;
                    } else {
                        m->fds[idx - 1].fp = fp;
                        m->fds[idx - 1].in_use = true;
                        open_fd = (u8)idx;
                        open_err = M4_OK;
                    }
                }
            }
        }
        err = open_err;
        resp_u8(m, &roff, open_fd);
        resp_u8(m, &roff, open_err);
        break;
    }

    case C_READ: {
        leds_ping_m4_disk();
        /* Request:  data[0]=fd, data[1..2]=size
         * Response: resp+3 = result (0=OK)
         *           resp+4 onwards = exactly `size` bytes (zero-padded on EOF).
         * On real hardware reading past EOF from a disk sector returns the
         * sector padding (zeros), and AMSDOS hands those zeros back to the
         * caller. The M4ROM's fread skips the destination copy entirely if
         * resp+3 is non-zero — so signalling EOF here would leave garbage
         * in the caller's load buffer and break programs (e.g. SymbOS) that
         * rely on the trailing padding being predictable. Always return 0
         * (success) and zero-fill the unread tail. */
        if (plen < 3) { err = M4_ERR_IO; break; }
        u8  fd    = p[0];
        u16 count = (u16)p[1] | ((u16)p[2] << 8);
        if (!valid_fd(m, fd)) {
            resp_u8(m, &roff, M4_ERR_BADFD);
            err = M4_ERR_BADFD;
            break;
        }
        resp_u8(m, &roff, M4_OK);
        u16 n = 0;
        if (m->fds[fd - 1].fatf) {
            u32 got = fat_read(m->fds[fd - 1].fatf, &m->bus_mem[roff], count);
            n   = (u16)got;
            roff = (u16)(roff + got);
        } else {
            for (; n < count; n++) {
                int c = fgetc(m->fds[fd - 1].fp);
                if (c == EOF) break;
                resp_u8(m, &roff, (u8)c);
            }
        }
        for (; n < count; n++)
            resp_u8(m, &roff, 0x00);
        err = M4_OK;
        break;
    }

    case C_READ2: {
        leds_ping_m4_disk();
        /* Same as C_READ but skips AMSDOS header detection. Used by
         * char_in for unbuffered reads.
         * Request:  data[0]=fd, data[1..2]=size
         * Response: resp+3 = status (0=OK, 20=EOF)
         *           resp+4..5 = actual bytes read (size_lo, size_hi)
         *           resp+8 onwards = data (note the +6/+7 gap) */
        if (plen < 1) { err = M4_ERR_IO; break; }
        u8  fd    = p[0];
        u16 count = (plen >= 3) ? ((u16)p[1] | ((u16)p[2] << 8)) : 1;
        if (!valid_fd(m, fd)) {
            resp_u8(m, &roff, M4_ERR_BADFD);
            err = M4_ERR_BADFD;
            break;
        }
        u8 *status_p = &m->bus_mem[3];
        u8 *size_p   = &m->bus_mem[4];
        *status_p = 0;
        size_p[0] = 0; size_p[1] = 0;
        roff = 8;  /* data starts at resp+8 per ROM expectation */
        u16 n = 0;
        if (m->fds[fd - 1].fatf) {
            u32 got = fat_read(m->fds[fd - 1].fatf, &m->bus_mem[roff], count);
            n   = (u16)got;
            roff = (u16)(roff + got);
        } else {
            for (; n < count; n++) {
                int c = fgetc(m->fds[fd - 1].fp);
                if (c == EOF) break;
                resp_u8(m, &roff, (u8)c);
            }
        }
        size_p[0] = n & 0xFF;
        size_p[1] = n >> 8;
        /* Partial reads (n < count) imply we hit EOF mid-block. AMSDOS
         * and BASIC's ASCII-tokenise loop both rely on the EOF status
         * to stop processing — without it BASIC continues reading the
         * uninitialised tail of the buffer and raises a spurious BREAK
         * after each ASCII .BAS load. */
        if (n < count) *status_p = 20;  /* EOF */
        err = M4_OK;
        break;
    }

    case C_WRITE: {
        leds_ping_m4_disk();
        if (plen < 1) { err = M4_ERR_IO; resp_u8(m, &roff, err); break; }
        u8 fd = p[0];
        if (!valid_fd(m, fd)) { err = M4_ERR_BADFD; resp_u8(m, &roff, err); break; }
        size_t actual = (size_t)(plen - 1);
        size_t written;
        if (m->fds[fd - 1].fatf)
            written = fat_write(m->fds[fd - 1].fatf, &p[1], (u32)actual);
        else
            written = fwrite(&p[1], 1, actual, m->fds[fd - 1].fp);
        err = (written == actual) ? M4_OK : M4_ERR_IO;
        resp_u8(m, &roff, err);
        break;
    }

    case C_WRITE2: {
        leds_ping_m4_disk();
        if (plen < 2) { err = M4_ERR_IO; resp_u8(m, &roff, err); break; }
        u8 fd = p[0];
        u8 ch = p[1];
        if (!valid_fd(m, fd)) { err = M4_ERR_BADFD; resp_u8(m, &roff, err); break; }
        if (m->fds[fd - 1].fatf)
            fat_write(m->fds[fd - 1].fatf, &ch, 1);
        else
            fputc(ch, m->fds[fd - 1].fp);
        err = M4_OK;
        resp_u8(m, &roff, err);
        break;
    }

    case C_CLOSE: {
        u8 fd = (plen >= 1) ? p[0] : 0;
        u8 close_res;
        if (valid_fd(m, fd)) {
            if (m->fds[fd - 1].fatf) {
                fat_close(m->fds[fd - 1].fatf);
                m->fds[fd - 1].fatf = NULL;
            }
            if (m->fds[fd - 1].fp) {
                fclose(m->fds[fd - 1].fp);
                m->fds[fd - 1].fp = NULL;
            }
            m->fds[fd - 1].in_use = false;
            err = M4_OK;
            close_res = 0x00;
        } else {
            /* Always BADFD for unknown fds (including fd=1/2). M4ROM's boot
             * sequence closes fd=1 and fd=2 expecting BADFD ("nothing was
             * open") and uses that to gate its banner/autoboot. Returning
             * M4_OK here breaks boot. The SymbOS daemon's cleanup closes on
             * fd=1/2 must tolerate BADFD too — that's what real M4
             * hardware returns. */
            err = M4_ERR_BADFD;
            close_res = 0xFF;
        }
        resp_u8(m, &roff, close_res);
        break;
    }

    case C_SEEK: {
        /* M4 protocol: data[0]=fd, data[1..4]=offset (32-bit LE). Absolute
         * seek (SET) only — there's no whence byte. M4ROM's fseek wrapper
         * sends exactly 5 bytes after the command header. */
        if (plen < 5) { err = M4_ERR_IO; resp_u8(m, &roff, err); break; }
        u8  fd  = p[0];
        u32 pos = (u32)p[1] | ((u32)p[2] << 8) | ((u32)p[3] << 16) | ((u32)p[4] << 24);
        if (!valid_fd(m, fd)) { err = M4_ERR_BADFD; resp_u8(m, &roff, err); break; }
        if (m->fds[fd - 1].fatf)
            err = fat_seek(m->fds[fd - 1].fatf, pos) ? M4_OK : M4_ERR_IO;
        else
            err = (fseek(m->fds[fd - 1].fp, (long)pos, SEEK_SET) == 0)
                  ? M4_OK : M4_ERR_IO;
        resp_u8(m, &roff, err);
        break;
    }

    case C_EOF: {
        u8 fd = (plen >= 1) ? p[0] : 0;
        if (!valid_fd(m, fd)) { err = M4_ERR_BADFD; break; }
        err = M4_OK;
        u8 eof_v;
        if (m->fds[fd - 1].fatf) {
            FatFile *ff = m->fds[fd - 1].fatf;
            eof_v = (fat_tell(ff) >= fat_file_size(ff)) ? 1 : 0;
        } else {
            eof_v = feof(m->fds[fd - 1].fp) ? 1 : 0;
        }
        resp_u8(m, &roff, eof_v);
        break;
    }

    case C_FTELL: {
        u8 fd = (plen >= 1) ? p[0] : 0;
        if (!valid_fd(m, fd)) { err = M4_ERR_BADFD; break; }
        u32 pos;
        if (m->fds[fd - 1].fatf) {
            pos = fat_tell(m->fds[fd - 1].fatf);
        } else {
            long lp = ftell(m->fds[fd - 1].fp);
            if (lp < 0) { err = M4_ERR_IO; break; }
            pos = (u32)lp;
        }
        err = M4_OK;
        resp_u32le(m, &roff, pos);
        break;
    }

    case C_FSIZE: {
        u8 fd = (plen >= 1) ? p[0] : 0;
        if (!valid_fd(m, fd)) { err = M4_ERR_BADFD; break; }
        u32 sz;
        if (m->fds[fd - 1].fatf) {
            sz = fat_file_size(m->fds[fd - 1].fatf);
        } else {
            long saved = ftell(m->fds[fd - 1].fp);
            fseek(m->fds[fd - 1].fp, 0, SEEK_END);
            long lp = ftell(m->fds[fd - 1].fp);
            fseek(m->fds[fd - 1].fp, saved, SEEK_SET);
            if (lp < 0) { err = M4_ERR_IO; break; }
            sz = (u32)lp;
        }
        err = M4_OK;
        resp_u32le(m, &roff, sz);
        break;
    }

    case C_SDREAD: {
        leds_ping_m4_disk();
        /* Raw block read from the SD card image.
         * Request:  data[0..3]=LBA (little-endian 32-bit), data[4]=num sectors
         * Response: resp+3 = status (0=OK, 3=not ready, 4=invalid param)
         *           resp+4 onwards = sector data (512 bytes per sector)
         * Only works in image mode; directory mode has no underlying sectors. */
        if (!m->image_fp || plen < 5) {
            resp_u8(m, &roff, 3); /* not ready */
            err = M4_OK;
            break;
        }
        u32 lba   = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
        u8  nsec  = p[4];
        if (nsec == 0 || nsec > 4) {
            resp_u8(m, &roff, 4); /* invalid parameter */
            err = M4_OK;
            break;
        }
        if (fseek(m->image_fp, (long)lba * 512, SEEK_SET) != 0) {
            resp_u8(m, &roff, 1); /* R/W error */
            err = M4_OK;
            break;
        }
        resp_u8(m, &roff, 0); /* success */
        size_t want = (size_t)nsec * 512;
        size_t got  = fread(&m->bus_mem[roff], 1, want, m->image_fp);
        roff = (u16)(roff + want);
        for (size_t i = got; i < want; i++)
            m->bus_mem[roff - want + i] = 0; /* pad past-EOF with zeros */
        err = M4_OK;
        break;
    }

    case C_SDWRITE: {
        leds_ping_m4_disk();
        /* Raw block write to the SD card image.
         * Request:  data[0..3]=LBA, data[4]=num sectors, data[5..]=sector data
         * Response: resp+3 = status (0=OK, 1=R/W err, 2=write-protected). */
        if (!m->image_fp || plen < 5) {
            resp_u8(m, &roff, 3);
            err = M4_OK;
            break;
        }
        if (m->image_read_only) {
            resp_u8(m, &roff, 2);
            err = M4_OK;
            break;
        }
        u32 lba  = (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
        u8  nsec = p[4];
        if (nsec == 0 || nsec > 4 || plen < 5 + (int)nsec * 512) {
            resp_u8(m, &roff, 4);
            err = M4_OK;
            break;
        }
        if (fseek(m->image_fp, (long)lba * 512, SEEK_SET) != 0
                || fwrite(&p[5], 1, (size_t)nsec * 512, m->image_fp)
                   != (size_t)nsec * 512) {
            resp_u8(m, &roff, 1);
            err = M4_OK;
            break;
        }
        fflush(m->image_fp);
        resp_u8(m, &roff, 0);
        err = M4_OK;
        break;
    }

    case C_ERASEFILE: {
        u8 erase_err = M4_ERR_IO;
        if (plen >= 1) {
            if (m4_use_fat(m)) {
                char abs[M4_PATH_MAX];
                fat_abs_path(m, (const char *)p, abs, sizeof(abs));
                erase_err = m->image_read_only ? M4_ERR_RDONLY :
                    (fat_delete(&m->image_vol, abs) ? M4_OK : M4_ERR_NOFILE);
            } else if (m->root[0]) {
                char hostpath[M4_PATH_MAX];
                if (resolve_path(m, (const char *)p, hostpath, sizeof(hostpath)))
                    erase_err = (remove(hostpath) == 0) ? M4_OK : M4_ERR_IO;
                else
                    erase_err = M4_ERR_NOFILE;
            }
        }
        resp_u8(m, &roff, erase_err);
        err = M4_OK;
        break;
    }

    case C_RENAME: {
        if (!m->root[0] || plen < 3) { err = M4_ERR_IO; break; }
        const char *oldname = (const char *)p;
        const char *newname = oldname + strlen(oldname) + 1;
        if ((int)(newname - (const char *)p) >= plen) { err = M4_ERR_IO; break; }
        char oldhost[M4_PATH_MAX], newhost[M4_PATH_MAX];
        if (!resolve_path(m, oldname, oldhost, sizeof(oldhost)) ||
            !resolve_path(m, newname, newhost, sizeof(newhost)))
            { err = M4_ERR_NOFILE; break; }
        err = (rename(oldhost, newhost) == 0) ? M4_OK : M4_ERR_IO;
        break;
    }

    case C_MAKEDIR: {
        if (!m->root[0] || plen < 1) { err = M4_ERR_IO; break; }
        char hostpath[M4_PATH_MAX];
        if (!resolve_path(m, (const char *)p, hostpath, sizeof(hostpath)))
            { err = M4_ERR_IO; break; }
        err = (mkdir(hostpath, 0755) == 0) ? M4_OK : M4_ERR_IO;
        break;
    }

    /* ---- Network — host-backed TCP via ESP8266 emulation ---- */

    case C_NETSTAT: {
        /* M4 protocol: text "network status string" + trailing status byte.
         * We're always "connected (and got IP)" in the emulator. */
        err = M4_OK;
        resp_str(m, &roff, "Connected (emulated host)");
        resp_u8(m, &roff, 5); /* status = connected */
        break;
    }

    case C_GETNETWORK: {
        /* Network configuration structure. M4ROM reads the six-byte MAC at
         * offsets 190..195, so the response must include all 196 bytes. */
        u8 cfg[196];
        memset(cfg, 0, sizeof(cfg));
        strncpy((char *)&cfg[0],   "1984",   16);
        strncpy((char *)&cfg[16],  "emulator", 32);
        cfg[112]=192; cfg[113]=168; cfg[114]=1; cfg[115]=100; /* ip */
        cfg[116]=255; cfg[117]=255; cfg[118]=255; cfg[119]=0; /* nm */
        cfg[120]=192; cfg[121]=168; cfg[122]=1; cfg[123]=1;   /* gw */
        cfg[124]=8;   cfg[125]=8;   cfg[126]=8;   cfg[127]=8; /* dns1 */
        cfg[128]=8;   cfg[129]=8;   cfg[130]=4;   cfg[131]=4; /* dns2 */
        cfg[132]=0;                                            /* dhcp off (static IP, we provide one) */
        cfg[190]=0x02; cfg[191]=0x19; cfg[192]=0x84;           /* locally administered MAC */
        cfg[193]=0x00; cfg[194]=0x00; cfg[195]=0x01;
        err = M4_OK;
        for (size_t i = 0; i < sizeof(cfg); i++) resp_u8(m, &roff, cfg[i]);
        break;
    }

    case C_SETNETWORK:
        /* Setup string accepted but ignored — emulator uses host TCP stack. */
        err = M4_OK;
        break;

    case C_NETRSSI:
        /* SymbOS daemon reads 2 bytes: low = RSSI level, high = wifi state.
         * Per the daemon source: 0=idle, 1=connecting, 2=wrong pw,
         * 3=no AP, 4=connection failed, 5=connected and got IP,
         * 6=unknown error. We always emulate "connected with IP" so the
         * Network daemon's status window shows the right thing. */
        err = M4_OK;
        resp_u8(m, &roff, 0xB8);  /* signal level in the "good" band */
        resp_u8(m, &roff, 5);     /* wifi state: connected and got IP */
        /* Snapshot the two payload bytes so a subsequent C_TIME (or any
         * other command) that lands between this ACK and the daemon's
         * m4cred LDIR doesn't replace them with its own response. */
        m->rssi_resp_save[0] = 0xB8;
        m->rssi_resp_save[1] = 5;
        m->rssi_resp_pending = true;
        break;

    case C_WIFIPOW:
        err = M4_OK;
        break;

    case C_NETSOCKET: {
        leds_ping_m4_net();
        /* Allocate a TCP socket. Return socket id 1..4 at resp+3, or 0xFF. */
        int idx = -1;
        for (int i = 1; i < M4_NSOCKS; i++)
            if (m->sockets[i].fd < 0) { idx = i; break; }
        u8 sid = 0xFF;
        if (idx > 0) {
            int s = socket(AF_INET, SOCK_STREAM, 0);
            if (s >= 0) {
                sock_set_nonblocking(s);
                int one = 1;
                setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof(one));
                m->sockets[idx].fd      = s;
                m->sockets[idx].status  = 0;
                m->sockets[idx].lastcmd = 0;
                m->sockets[idx].rx_count = 0;
                m->sockets[idx].telnet_mode = false;
                sync_sock_mem(m, idx);
                sid = (u8)idx;
                m->last_tcp_sock = idx;
            }
        }
        err = M4_OK;
        resp_u8(m, &roff, sid);
        break;
    }

    case C_NETCONNECT: {
        leds_ping_m4_net();
        u8 ok = 0xFF;
        if (plen >= 7) {
            int s = p[0];
            /* SymbOS daemon workaround: it appears to pass arbitrary garbage
             * as the M4 socket on TCP_RECV calls (root cause TBD — looks like
             * m4cscktrn gets clobbered between CONNECT and the first RECV
             * after data starts arriving). Any value that doesn't resolve to
             * a valid open TCP socket of ours is routed to the most-recently-
             * opened one, which is unambiguous because we never open more
             * than one concurrently in this flow. */
            if ((s <= 0 || s >= M4_NSOCKS || m->sockets[s].fd < 0)
                    && m->last_tcp_sock > 0
                    && m->sockets[m->last_tcp_sock].fd >= 0)
                s = m->last_tcp_sock;
            if (s > 0 && s < M4_NSOCKS && m->sockets[s].fd >= 0) {
                struct sockaddr_in sa = {0};
                sa.sin_family = AF_INET;
                memcpy(&sa.sin_addr.s_addr, &p[1], 4);
                sa.sin_port = htons((u16)p[5] | ((u16)p[6] << 8));
                memcpy(m->sockets[s].peer_ip, &p[1], 4);
                m->sockets[s].peer_port = (u16)p[5] | ((u16)p[6] << 8);
                m->sockets[s].telnet_mode = (m->sockets[s].peer_port == 23);
                m->sockets[s].lastcmd = 3;
                int r = connect(m->sockets[s].fd, (struct sockaddr *)&sa, sizeof(sa));
                bool inprog = sock_in_progress();
                if (r == 0 || inprog) {
                    /* Keep the socket in the connecting state until the next
                     * poll tick so the SymbOS daemon observes a real status
                     * transition and emits NET_TCPEVT. If connect() completed
                     * immediately, collapsing straight to status=0 can skip
                     * the event path and leave TCP_OpenWait spinning. */
                    m->sockets[s].connecting = true;
                    m->sockets[s].status = 1;
                    ok = 0;
                } else {
                    m->sockets[s].status = 240;
                }
                sync_sock_mem(m, s);
            }
        }
        err = M4_OK;
        resp_u8(m, &roff, ok);
        break;
    }

    case C_NETCLOSE: {
        leds_ping_m4_net();
        if (plen >= 1) {
            int s = p[0];
            /* SymbOS daemon workaround: it appears to pass arbitrary garbage
             * as the M4 socket on TCP_RECV calls (root cause TBD — looks like
             * m4cscktrn gets clobbered between CONNECT and the first RECV
             * after data starts arriving). Any value that doesn't resolve to
             * a valid open TCP socket of ours is routed to the most-recently-
             * opened one, which is unambiguous because we never open more
             * than one concurrently in this flow. */
            if ((s <= 0 || s >= M4_NSOCKS || m->sockets[s].fd < 0)
                    && m->last_tcp_sock > 0
                    && m->sockets[m->last_tcp_sock].fd >= 0)
                s = m->last_tcp_sock;
            net_close_socket(m, s);
            if (s == m->last_tcp_sock) m->last_tcp_sock = 0;
        }
        err = M4_OK;
        resp_u8(m, &roff, 0);
        break;
    }

    case C_NETSEND: {
        leds_ping_m4_net();
        u8 ok = 0xFF;
        if (plen >= 3) {
            int s = p[0];
            /* SymbOS daemon workaround: it appears to pass arbitrary garbage
             * as the M4 socket on TCP_RECV calls (root cause TBD — looks like
             * m4cscktrn gets clobbered between CONNECT and the first RECV
             * after data starts arriving). Any value that doesn't resolve to
             * a valid open TCP socket of ours is routed to the most-recently-
             * opened one, which is unambiguous because we never open more
             * than one concurrently in this flow. */
            if ((s <= 0 || s >= M4_NSOCKS || m->sockets[s].fd < 0)
                    && m->last_tcp_sock > 0
                    && m->sockets[m->last_tcp_sock].fd >= 0)
                s = m->last_tcp_sock;
            u16 sz = (u16)p[1] | ((u16)p[2] << 8);
            if (s > 0 && s < M4_NSOCKS && m->sockets[s].fd >= 0
                    && (int)sz <= plen - 3) {
                ssize_t n = send(m->sockets[s].fd, (const char *)&p[3], sz, MSG_NOSIGNAL);
                if (n == (ssize_t)sz) ok = 0;
                m->sockets[s].lastcmd = 1;
                sync_sock_mem(m, s);
            }
        }
        err = M4_OK;
        resp_u8(m, &roff, ok);
        break;
    }

    case C_NETRECV: {
        leds_ping_m4_net();
        /* Response: resp+3 = result (0=OK), resp+4..5 = actual size,
         *           resp+6.. = data (up to 2KB). Both cpc-sdcc's net_recv
         *           and the SymbOS daemon's m4ctrx (which fetches from
         *           m4crombuf+3+3 = resp+6 via m4crcv) expect the data
         *           payload to start at offset 6 with no padding gap. */
        u16 actual = 0;
        u8  result = 0xFF;
        if (plen >= 3) {
            int s = p[0];
            /* SymbOS daemon workaround: it appears to pass arbitrary garbage
             * as the M4 socket on TCP_RECV calls (root cause TBD — looks like
             * m4cscktrn gets clobbered between CONNECT and the first RECV
             * after data starts arriving). Any value that doesn't resolve to
             * a valid open TCP socket of ours is routed to the most-recently-
             * opened one, which is unambiguous because we never open more
             * than one concurrently in this flow. */
            if ((s <= 0 || s >= M4_NSOCKS || m->sockets[s].fd < 0)
                    && m->last_tcp_sock > 0
                    && m->sockets[m->last_tcp_sock].fd >= 0)
                s = m->last_tcp_sock;
            u16 want = (u16)p[1] | ((u16)p[2] << 8);
            if (want > 0x800) want = 0x800;
            if (s > 0 && s < M4_NSOCKS && m->sockets[s].fd >= 0) {
                ssize_t n = recv(m->sockets[s].fd, (char *)&m->bus_mem[6], want,
                                 MSG_DONTWAIT);
                if (n >= 0) {
                    actual = (u16)n;
                    if (actual > 0 && m->sockets[s].telnet_mode)
                        actual = telnet_filter_inplace(&m->bus_mem[6], actual);
                    result = 0;
                } else if (sock_would_block()) {
                    actual = 0;
                    result = 0;
                }
                m->sockets[s].lastcmd = 5;
                if (n == 0) m->sockets[s].status = 3; /* remote closed */
                m->sockets[s].rx_count = (m->sockets[s].rx_count > actual)
                                          ? (u16)(m->sockets[s].rx_count - actual) : 0;
                sync_sock_mem(m, s);
            }
        }
        m->bus_mem[3] = result;
        m->bus_mem[4] = (u8)(actual & 0xFF);
        m->bus_mem[5] = (u8)(actual >> 8);
        roff = (u16)(6 + actual);
        err = M4_OK;
        break;
    }

    case C_NETHOSTIP: {
        leds_ping_m4_net();
        /* Synchronous resolution into socket 0's sock_info entry. */
        if (plen < 1) { err = M4_OK; resp_u8(m, &roff, 0xFF); break; }
        char host[256];
        size_t hl = (size_t)plen < sizeof(host) - 1 ? (size_t)plen : sizeof(host) - 1;
        memcpy(host, p, hl);
        host[hl] = '\0';
        /* Strip trailing NULs from the M4 protocol terminator */
        while (hl > 0 && host[hl-1] == '\0') hl--;
        host[hl] = '\0';

        struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
        struct addrinfo *res = NULL;
        if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
            struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
            memcpy(m->sockets[0].peer_ip, &sa->sin_addr.s_addr, 4);
            m->sockets[0].status = 0;
            freeaddrinfo(res);
        } else {
            m->sockets[0].status = 0xF0; /* error */
        }
        m->sockets[0].lastcmd = 2;
        sync_sock_mem(m, 0);
        err = M4_OK;
        resp_u8(m, &roff, 1);  /* "lookup in progress / done — read sockinfo" */
        break;
    }

    case C_NETBIND:
    case C_NETLISTEN:
    case C_NETACCEPT:
    case C_HTTPGET:
    case C_HTTPGETMEM:
        err = M4_ERR_NOTSUP;
        break;

    case C_READMEM: {
        /* Drain bytes out of the M4's internal buffers — used by netd-1984
         * to poll sock_info[s] between commands. Args: addr-lo, addr-hi,
         * length. Response: 'length' bytes copied from the matching buffer. */
        if (plen < 3) { err = M4_ERR_IO; break; }
        u16 addr = (u16)p[0] | ((u16)p[1] << 8);
        u16 n    = (u16)p[2];
        const u8 *src = NULL; u16 cap = 0;
        if (addr >= 0xE800 && addr < 0xF400) {
            src = &m->bus_mem[addr - 0xE800];
            cap = (u16)(0xF400 - addr);
        } else if (addr >= 0xF400 && addr < 0xF500) {
            src = &m->cfg_mem[addr - 0xF400];
            cap = (u16)(0xF500 - addr);
        } else if (addr >= 0xFE00 && addr < 0xFE50) {
            src = &m->sock_mem[addr - 0xFE00];
            cap = (u16)(0xFE50 - addr);
        }
        if (!src) { err = M4_ERR_IO; break; }
        if (n > cap) n = cap;
        for (u16 i = 0; i < n; i++) resp_u8(m, &roff, src[i]);
        err = M4_OK;
        break;
    }

    default:
        err = M4_ERR_NOTSUP;
        break;
    }

    m->last_error = err;
    resp_frame(m, cmd, roff);
    /* Track how many bytes this command's caller is expected to read so
     * we know when it's safe to restore a pending RSSI snapshot (see
     * bus_mem_read in cpc.c). */
    m->last_resp_len = (roff > 3) ? (int)(roff - 3) : 0;
    /* Network clients such as SymbOS read responses while another upper ROM
     * is selected. Keep M4 RAM mode alive for the complete response, not just
     * the small sock_info-sized transfers. C_GETNETWORK alone returns 196
     * bytes; the old fixed 24-read budget exposed only its prefix and made
     * netd-m4c.exe abort during startup. RAM mode is cleared when the caller
     * consumes the response or when the next command supersedes it. */
    if (m->ram_mode && m->ram_mode_reads < m->last_resp_len + 16)
        m->ram_mode_reads = m->last_resp_len + 16;
    if (m4_trace) {
        int rlen = (roff > 3) ? (roff - 3) : 0;
        fprintf(stderr, "[m4]      RSP err=%02X (%d payload):", err, rlen);
        for (int i = 0; i < rlen && i < 16; i++)
            fprintf(stderr, " %02X", m->bus_mem[3 + i]);
        if (rlen > 16) fprintf(stderr, " ...(+%d)", rlen - 16);
        fprintf(stderr, "\n");
        if (m->ram_mode)
            fprintf(stderr, "[m4]      RAMMODE reads=%d\n", m->ram_mode_reads);
    }
    m->cmd_len = 0;
    return m->nmi_enabled;
}

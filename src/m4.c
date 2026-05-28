#define _GNU_SOURCE
#include "m4.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <fnmatch.h>
#include <limits.h>
#include <time.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <libgen.h>

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
#define C_NETBIND     0x4338
#define C_NETLISTEN   0x4339
#define C_NETACCEPT   0x433A
#define C_GETNETWORK  0x433B
#define C_WIFIPOW     0x433C
#define C_CONFIG      0x43FE

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

/* Write error at base, return for use in switch cases */
static void resp_err(M4 *m, u8 err) {
    m->bus_mem[0] = err;
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

/* Open the raw disk image file if image_path is set. */
static void m4_open_image(M4 *m) {
    if (m->image_fp) { fclose(m->image_fp); m->image_fp = NULL; }
    if (!m->image_path[0]) return;
    struct stat st;
    if (stat(m->image_path, &st) != 0 || !S_ISREG(st.st_mode)) return;
    m->image_fp = fopen(m->image_path, "r+b");
    if (!m->image_fp) m->image_fp = fopen(m->image_path, "rb"); /* read-only fallback */
}

void m4_init(M4 *m, const char *root) {
    memset(m, 0, sizeof(*m));
    m->nmi_enabled = false;  /* ROM enables NMI explicitly via C_NMION after init */
    snprintf(m->cwd, sizeof(m->cwd), "/");
    if (root && root[0])
        snprintf(m->root, sizeof(m->root), "%s", root);
    snprintf(m->dir_filter, sizeof(m->dir_filter), "*");
}

void m4_set_image(M4 *m, const char *image_path) {
    if (image_path && image_path[0])
        snprintf(m->image_path, sizeof(m->image_path), "%s", image_path);
    else
        m->image_path[0] = '\0';
    m4_open_image(m);
}

void m4_reset(M4 *m) {
    /* Close all open files and directories, reset command buffer and cwd */
    for (int i = 0; i < M4_MAX_FDS; i++) {
        if (m->fds[i].in_use && m->fds[i].fp)
            fclose(m->fds[i].fp);
        m->fds[i].in_use = false;
        m->fds[i].fp = NULL;
    }
    if (m->dir_dp) { closedir(m->dir_dp); m->dir_dp = NULL; }
    m->cmd_len = 0;
    snprintf(m->cwd, sizeof(m->cwd), "/");
    snprintf(m->dir_filter, sizeof(m->dir_filter), "*");
    /* Re-open image fd (if image_path is set) */
    m4_open_image(m);
    m->nmi_enabled = false;
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

bool m4_ackport_write(M4 *m, Mem *mem) {
    if (m->cmd_len < 3) {
        resp_err(m, M4_ERR_IO);
        m->cmd_len = 0;
        return m->nmi_enabled;
    }

    u16 cmd = (u16)m->cmd_buf[1] | ((u16)m->cmd_buf[2] << 8);
    const u8 *p = m->cmd_buf + 3;   /* params */
    int plen    = m->cmd_len - 3;
    u8  err     = M4_ERR_NOTSUP;
    u16 roff    = 3;                 /* response data written starting at RESP_BASE+3 (M4 protocol) */

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
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        err = M4_OK;
        /* year (2-digit), month (1-12), day, hour, min, sec, day-of-week (0=Sun) */
        resp_u8(m, &roff, (u8)(tm->tm_year % 100));
        resp_u8(m, &roff, (u8)(tm->tm_mon + 1));
        resp_u8(m, &roff, (u8)tm->tm_mday);
        resp_u8(m, &roff, (u8)tm->tm_hour);
        resp_u8(m, &roff, (u8)tm->tm_min);
        resp_u8(m, &roff, (u8)tm->tm_sec);
        resp_u8(m, &roff, (u8)tm->tm_wday);
        break;
    }

    case C_NMIOFF:
        m->nmi_enabled = (plen > 0) ? (p[0] == 0) : false;
        err = M4_OK;
        break;

    case C_M4OFF:
    case C_ROMSOFF:
    case C_RAMDISOFF:
        err = M4_OK;
        break;

    /* ---- Filesystem navigation ---- */

    case C_GETPATH:
        if (!m->root[0]) { err = M4_ERR_IO; break; }
        err = M4_OK;
        resp_str(m, &roff, m->cwd);
        break;

    case C_CD: {
        if (!m->root[0] || plen < 1) { err = M4_ERR_IO; break; }
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
        char hostpath[M4_PATH_MAX];
        if (!resolve_path(m, path, hostpath, sizeof(hostpath))) { err = M4_ERR_NOFILE; break; }
        struct stat st;
        if (stat(hostpath, &st) != 0 || !S_ISDIR(st.st_mode)) { err = M4_ERR_NOFILE; break; }
        /* Update cwd to path relative to root */
        const char *rel = hostpath + strlen(m->root);
        snprintf(m->cwd, sizeof(m->cwd), "%s", rel[0] ? rel : "/");
        err = M4_OK;
        break;
    }

    case C_FREE: {
        u32 free_kb = 0;
        if (m->root[0]) {
            struct statvfs sv;
            if (statvfs(m->root, &sv) == 0)
                free_kb = (u32)((u64)sv.f_bavail * sv.f_bsize / 1024);
        }
        char buf[40];
        snprintf(buf, sizeof(buf), "\r\n%uK free\r\n\r\n", (unsigned)free_kb);
        err = M4_OK;
        resp_str(m, &roff, buf);
        break;
    }

    /* ---- Directory listing ---- */

    case C_DIRSETARGS: {
        if (!m->root[0]) { err = M4_ERR_IO; break; }
        if (m->dir_dp) { closedir(m->dir_dp); m->dir_dp = NULL; }

        /* params: optional filter string */
        if (plen > 0 && p[0])
            snprintf(m->dir_filter, sizeof(m->dir_filter), "%s", (const char *)p);
        else
            snprintf(m->dir_filter, sizeof(m->dir_filter), "*");

        char hostpath[M4_PATH_MAX];
        snprintf(hostpath, sizeof(hostpath), "%s%s", m->root, m->cwd);
        m->dir_dp = opendir(hostpath);
        err = m->dir_dp ? M4_OK : M4_ERR_IO;
        break;
    }

    case C_READDIR: {
        /* M4ROM's catalog loop checks rom_response+0 == 2 for EOF, so the
         * EOF marker for THIS command is fixed at 2 regardless of our
         * generic error map. */
        if (!m->dir_dp) { err = 2; break; }

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

        /* Get file info */
        char hostpath[M4_PATH_MAX];
        snprintf(hostpath, sizeof(hostpath), "%s%s/%s",
                 m->root, m->cwd, de->d_name);
        struct stat st;
        bool is_dir = false;
        u32  fsize  = 0;
        if (stat(hostpath, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            fsize  = is_dir ? 0 : (u32)st.st_size;
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
        const char *src = de->d_name;
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
        if (m->root[0] && plen >= 2) {
            u8 mode = p[0];
            const char *name = (const char *)&p[1];
            char hostpath[M4_PATH_MAX];
            bool is_write = (mode & 0x02) != 0;
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
                const char *fmode = (mode & 0x02) ? "wb" : (mode & 0x30) ? "ab" : "rb";
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
                    FILE *fp = fopen(hostpath, fmode);
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
        for (; n < count; n++) {
            int c = fgetc(m->fds[fd - 1].fp);
            if (c == EOF) break;
            resp_u8(m, &roff, (u8)c);
        }
        for (; n < count; n++)
            resp_u8(m, &roff, 0x00);
        err = M4_OK;
        break;
    }

    case C_READ2: {
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
        for (; n < count; n++) {
            int c = fgetc(m->fds[fd - 1].fp);
            if (c == EOF) break;
            resp_u8(m, &roff, (u8)c);
        }
        size_p[0] = n & 0xFF;
        size_p[1] = n >> 8;
        if (n == 0) *status_p = 20;  /* EOF */
        err = M4_OK;
        break;
    }

    case C_WRITE: {
        /* M4 protocol: data[0] = fd, data[1..] = raw bytes to write.
         * The payload length is implicit (whole packet minus header+fd). */
        if (plen < 1) { err = M4_ERR_IO; break; }
        u8 fd = p[0];
        if (!valid_fd(m, fd)) { err = M4_ERR_BADFD; break; }
        size_t actual = (size_t)(plen - 1);
        size_t written = fwrite(&p[1], 1, actual, m->fds[fd - 1].fp);
        err = (written == actual) ? M4_OK : M4_ERR_IO;
        break;
    }

    case C_WRITE2: {
        /* Write single byte */
        if (plen < 2) { err = M4_ERR_IO; break; }
        u8 fd = p[0];
        u8 ch = p[1];
        if (!valid_fd(m, fd)) { err = M4_ERR_BADFD; break; }
        fputc(ch, m->fds[fd - 1].fp);
        err = M4_OK;
        break;
    }

    case C_CLOSE: {
        u8 fd = (plen >= 1) ? p[0] : 0;
        u8 close_res;
        if (valid_fd(m, fd)) {
            fclose(m->fds[fd - 1].fp);
            m->fds[fd - 1].fp = NULL;
            m->fds[fd - 1].in_use = false;
            err = M4_OK;
            close_res = 0x00;  /* success */
        } else {
            err = M4_ERR_BADFD;
            close_res = 0xFF;  /* fd not open — triggers init_count check in M4ROM's fclose */
        }
        resp_u8(m, &roff, close_res);
        break;
    }

    case C_SEEK: {
        if (plen < 6) { err = M4_ERR_IO; break; }
        u8  fd  = p[0];
        u32 pos = (u32)p[1] | ((u32)p[2] << 8) | ((u32)p[3] << 16) | ((u32)p[4] << 24);
        int wh  = (int)p[5]; /* 0=SET, 1=CUR, 2=END */
        if (!valid_fd(m, fd)) { err = M4_ERR_BADFD; break; }
        int whence = (wh == 1) ? SEEK_CUR : (wh == 2) ? SEEK_END : SEEK_SET;
        err = (fseek(m->fds[fd - 1].fp, (long)pos, whence) == 0) ? M4_OK : M4_ERR_IO;
        break;
    }

    case C_EOF: {
        u8 fd = (plen >= 1) ? p[0] : 0;
        if (!valid_fd(m, fd)) { err = M4_ERR_BADFD; break; }
        err = M4_OK;
        resp_u8(m, &roff, feof(m->fds[fd - 1].fp) ? 1 : 0);
        break;
    }

    case C_FTELL: {
        u8 fd = (plen >= 1) ? p[0] : 0;
        if (!valid_fd(m, fd)) { err = M4_ERR_BADFD; break; }
        long pos = ftell(m->fds[fd - 1].fp);
        if (pos < 0) { err = M4_ERR_IO; break; }
        err = M4_OK;
        resp_u32le(m, &roff, (u32)pos);
        break;
    }

    case C_FSIZE: {
        u8 fd = (plen >= 1) ? p[0] : 0;
        if (!valid_fd(m, fd)) { err = M4_ERR_BADFD; break; }
        long saved = ftell(m->fds[fd - 1].fp);
        fseek(m->fds[fd - 1].fp, 0, SEEK_END);
        long sz = ftell(m->fds[fd - 1].fp);
        fseek(m->fds[fd - 1].fp, saved, SEEK_SET);
        if (sz < 0) { err = M4_ERR_IO; break; }
        err = M4_OK;
        resp_u32le(m, &roff, (u32)sz);
        break;
    }

    case C_SDREAD: {
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
        /* Raw block write to the SD card image.
         * Request:  data[0..3]=LBA, data[4]=num sectors, data[5..]=sector data
         * Response: resp+3 = status (0=OK, 1=R/W err, 2=write-protected). */
        if (!m->image_fp || plen < 5) {
            resp_u8(m, &roff, 3);
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
        if (!m->root[0] || plen < 1) { err = M4_ERR_IO; break; }
        char hostpath[M4_PATH_MAX];
        if (!resolve_path(m, (const char *)p, hostpath, sizeof(hostpath)))
            { err = M4_ERR_NOFILE; break; }
        err = (remove(hostpath) == 0) ? M4_OK : M4_ERR_IO;
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

    /* ---- Network stubs (Phase 2) ---- */
    case C_NETSOCKET:
    case C_NETCONNECT:
    case C_NETCLOSE:
    case C_NETSEND:
    case C_NETRECV:
    case C_NETHOSTIP:
    case C_NETBIND:
    case C_NETLISTEN:
    case C_NETACCEPT:
    case C_NETSTAT:
    case C_NETRSSI:
    case C_GETNETWORK:
    case C_SETNETWORK:
    case C_WIFIPOW:
    case C_HTTPGET:
    case C_HTTPGETMEM:
        err = M4_ERR_NOTSUP;
        break;

    default:
        err = M4_ERR_NOTSUP;
        break;
    }

    resp_err(m, err);
    m->cmd_len = 0;
    return m->nmi_enabled;
}

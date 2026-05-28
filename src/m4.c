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

/* Response base address in CPC RAM */
#define RESP_BASE 0xE800u

/* ---- Response helpers ---- */

static void resp_u8(Mem *mem, u16 *off, u8 v) {
    mem_write(mem, RESP_BASE + (*off)++, v);
}
static void resp_u16le(Mem *mem, u16 *off, u16 v) {
    resp_u8(mem, off, v & 0xFF);
    resp_u8(mem, off, v >> 8);
}
static void resp_u32le(Mem *mem, u16 *off, u32 v) {
    resp_u8(mem, off, v & 0xFF);
    resp_u8(mem, off, (v >> 8) & 0xFF);
    resp_u8(mem, off, (v >> 16) & 0xFF);
    resp_u8(mem, off, v >> 24);
}
static void resp_str(Mem *mem, u16 *off, const char *s) {
    while (*s) resp_u8(mem, off, (u8)*s++);
    resp_u8(mem, off, 0);
}

/* Write error at base, return for use in switch cases */
static void resp_err(Mem *mem, u8 err) {
    mem_write(mem, RESP_BASE, err);
}

/* ---- Path helpers ---- */

/* Build a host filesystem path from an M4 CPC path (absolute or relative to cwd).
 * Ensures the resolved path stays within m->root. Returns true on success. */
static bool resolve_path(const M4 *m, const char *cpc_path, char *out, size_t outsz) {
    if (!m->root[0]) return false;

    char combined[M4_PATH_MAX * 2];
    /* Replace backslashes with forward slashes */
    char clean[M4_PATH_MAX];
    size_t ci = 0;
    for (size_t i = 0; cpc_path[i] && ci < sizeof(clean) - 1; i++)
        clean[ci++] = (cpc_path[i] == '\\') ? '/' : cpc_path[i];
    clean[ci] = '\0';

    if (clean[0] == '/') {
        snprintf(combined, sizeof(combined), "%s%s", m->root, clean);
    } else {
        snprintf(combined, sizeof(combined), "%s%s/%s", m->root, m->cwd, clean);
    }

    char resolved[PATH_MAX];
    if (!realpath(combined, resolved)) {
        /* File doesn't exist yet (for writes); try parent */
        char tmp[PATH_MAX];
        snprintf(tmp, sizeof(tmp), "%s", combined);
        char *slash = strrchr(tmp, '/');
        if (!slash) return false;
        *slash = '\0';
        char parent[PATH_MAX];
        if (!realpath(tmp, parent)) return false;
        snprintf(resolved, sizeof(resolved), "%s/%s", parent, slash + 1);
    }

    /* Security: must stay within root */
    size_t rootlen = strlen(m->root);
    if (strncmp(resolved, m->root, rootlen) != 0) return false;

    snprintf(out, outsz, "%s", resolved);
    return true;
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

void m4_init(M4 *m, const char *root) {
    memset(m, 0, sizeof(*m));
    m->nmi_enabled = false;  /* ROM enables NMI explicitly via C_NMION after init */
    snprintf(m->cwd, sizeof(m->cwd), "/");
    if (root && root[0])
        snprintf(m->root, sizeof(m->root), "%s", root);
    snprintf(m->dir_filter, sizeof(m->dir_filter), "*");
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
        resp_err(mem, M4_ERR_IO);
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

    case C_CONFIG:
        /* Config read/write. Packet: [offset][...data...].
         * Offset 0: init_rom workspace — extract jump vector (params[3-4]).
         * Patch the ROM content at jump_vec (0xF402, file offset 0x3402) so
         * set_SDdrive's ldi reads the correct fio_jvec address rather than the
         * ROM default of 0x0000. Also write rom_num (slot) at 0xF404. */
        if (plen >= 6 && p[0] == 0) {
            /* Offset 0: init_rom workspace — fio_jvec address and slot.
             * Write to CPC RAM at jump_vec (0xF402) so set_SDdrive's ldi
             * reads the correct dispatch vector via the bus bypass. */
            mem->ram[0xF402] = p[3];   /* jvec_lo */
            mem->ram[0xF403] = p[4];   /* jvec_hi */
            mem->ram[0xF404] = p[5];   /* slot (rom_num) */
        } else if (plen >= 2 && p[0] == 5) {
            /* Offset 5: init_count update. */
            m->init_count = p[1];
            mem->ram[0xF405] = m->init_count;
        }
        err = M4_OK;
        break;

    case C_VERSION:
        err = M4_OK;
        resp_str(mem, &roff, "M4 1984 Emulator");
        break;

    case C_TIME: {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        err = M4_OK;
        /* year (2-digit), month (1-12), day, hour, min, sec, day-of-week (0=Sun) */
        resp_u8(mem, &roff, (u8)(tm->tm_year % 100));
        resp_u8(mem, &roff, (u8)(tm->tm_mon + 1));
        resp_u8(mem, &roff, (u8)tm->tm_mday);
        resp_u8(mem, &roff, (u8)tm->tm_hour);
        resp_u8(mem, &roff, (u8)tm->tm_min);
        resp_u8(mem, &roff, (u8)tm->tm_sec);
        resp_u8(mem, &roff, (u8)tm->tm_wday);
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
        resp_str(mem, &roff, m->cwd);
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
        if (!m->root[0]) { err = M4_ERR_IO; break; }
        struct statvfs sv;
        if (statvfs(m->root, &sv) != 0) { err = M4_ERR_IO; break; }
        u32 free_kb = (u32)((u64)sv.f_bavail * sv.f_bsize / 1024);
        err = M4_OK;
        resp_u32le(mem, &roff, free_kb);
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
        if (!m->dir_dp) { err = M4_ERR_EOF; break; }

        struct dirent *de = NULL;
        for (;;) {
            de = readdir(m->dir_dp);
            if (!de) { err = M4_ERR_EOF; goto readdir_done; }
            /* Skip hidden dot entries, keep . and .. only if no filter */
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            /* Apply filter */
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
        u16  fdate  = 0, ftime = 0;
        if (stat(hostpath, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            fsize  = is_dir ? 0 : (u32)st.st_size;
            struct tm *tm = localtime(&st.st_mtime);
            if (tm) {
                int yr = tm->tm_year + 1900 - 1980;
                if (yr < 0) yr = 0;
                fdate = (u16)(((yr & 0x7F) << 9) |
                              (((tm->tm_mon + 1) & 0x0F) << 5) |
                              (tm->tm_mday & 0x1F));
                ftime = (u16)(((tm->tm_hour & 0x1F) << 11) |
                              ((tm->tm_min  & 0x3F) << 5) |
                              ((tm->tm_sec / 2) & 0x1F));
            }
        }

        /* Name: 13 bytes (8.3 max), null-terminated, padded with spaces */
        char name13[13];
        memset(name13, ' ', 12);
        name13[12] = '\0';
        strncpy(name13, de->d_name, 12);
        name13[12] = '\0';

        err = M4_OK;
        for (int i = 0; i < 13; i++)
            resp_u8(mem, &roff, (u8)name13[i]);
        resp_u8(mem,  &roff, is_dir ? 0x10 : 0x20); /* attr */
        resp_u32le(mem, &roff, fsize);
        resp_u16le(mem, &roff, fdate);
        resp_u16le(mem, &roff, ftime);
        readdir_done:;
        break;
    }

    /* ---- File I/O ---- */

    case C_OPEN: {
        /* ROM reads: resp+3 = fd, resp+4 = 0 means success / non-zero means error */
        u8 open_fd = 0xFF, open_err = M4_ERR_IO;
        if (m->root[0] && plen >= 2) {
            u8 mode = p[0];
            const char *name = (const char *)&p[1];
            char hostpath[M4_PATH_MAX];
            if (!resolve_path(m, name, hostpath, sizeof(hostpath))) {
                open_err = M4_ERR_NOFILE;
            } else {
                const char *fmode = (mode & 0x02) ? "wb" : (mode & 0x30) ? "ab" : "rb";
                int idx = alloc_fd(m);
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
        resp_u8(mem, &roff, open_fd);  /* fd at resp+3 */
        resp_u8(mem, &roff, open_err); /* error indicator at resp+4 (ROM checks this: 0=OK) */
        break;
    }

    case C_READ: {
        if (plen < 3) { err = M4_ERR_IO; break; }
        u8  fd    = p[0];
        u16 count = (u16)p[1] | ((u16)p[2] << 8);
        if (!valid_fd(m, fd)) { err = M4_ERR_BADFD; break; }

        /* Write byte count placeholder, then data */
        u16 cnt_off = roff;
        roff += 2;
        u16 n = 0;
        for (; n < count; n++) {
            int c = fgetc(m->fds[fd - 1].fp);
            if (c == EOF) break;
            resp_u8(mem, &roff, (u8)c);
        }
        /* Fill in actual count */
        mem_write(mem, RESP_BASE + cnt_off,     n & 0xFF);
        mem_write(mem, RESP_BASE + cnt_off + 1, n >> 8);
        err = M4_OK;
        break;
    }

    case C_READ2: {
        /* Read a single character from fd */
        u8 fd = (plen >= 3) ? p[2] : (plen >= 1 ? p[0] : 0);
        if (!valid_fd(m, fd)) { err = M4_ERR_BADFD; break; }
        int c = fgetc(m->fds[fd - 1].fp);
        if (c == EOF) { err = M4_ERR_EOF; break; }
        err = M4_OK;
        resp_u8(mem, &roff, (u8)c);
        break;
    }

    case C_WRITE: {
        if (plen < 3) { err = M4_ERR_IO; break; }
        u8  fd    = p[0];
        u16 count = (u16)p[1] | ((u16)p[2] << 8);
        if (!valid_fd(m, fd)) { err = M4_ERR_BADFD; break; }
        u16 actual = (u16)plen - 3;
        if (actual > count) actual = count;
        size_t written = fwrite(&p[3], 1, actual, m->fds[fd - 1].fp);
        err = M4_OK;
        resp_u16le(mem, &roff, (u16)written);
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
        if (!valid_fd(m, fd)) { err = M4_ERR_BADFD; break; }
        fclose(m->fds[fd - 1].fp);
        m->fds[fd - 1].fp = NULL;
        m->fds[fd - 1].in_use = false;
        err = M4_OK;
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
        resp_u8(mem, &roff, feof(m->fds[fd - 1].fp) ? 1 : 0);
        break;
    }

    case C_FTELL: {
        u8 fd = (plen >= 1) ? p[0] : 0;
        if (!valid_fd(m, fd)) { err = M4_ERR_BADFD; break; }
        long pos = ftell(m->fds[fd - 1].fp);
        if (pos < 0) { err = M4_ERR_IO; break; }
        err = M4_OK;
        resp_u32le(mem, &roff, (u32)pos);
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
        resp_u32le(mem, &roff, (u32)sz);
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

    resp_err(mem, err);
    m->cmd_len = 0;
    return m->nmi_enabled;
}

/* F10 host-mount feature — see issue #142.
 *
 * Pauses the emulator and mounts every active FAT card image on the host
 * so the user can drag files in / out from the file manager. On close,
 * unmount + cold-boot drops the guest's stale FAT cache.
 *
 * Two mount backends, tried in this order:
 *
 *   1. udisksctl loop-setup + udisksctl mount.
 *      Mount appears under /run/media/$USER/<label or UUID>/ as a
 *      first-class removable volume that Nautilus, Files, Dolphin, etc.
 *      treat natively — drag/drop works in the GUI without any FUSE
 *      caveats. Needs udisks2 (default on every modern desktop).
 *
 *   2. libguestfs guestmount.
 *      Works without a session bus / polkit, useful for headless setups
 *      and minimal installs. Mount is in ~/.cache/1984/mounts/<label>/.
 *      Note that GNOME Files refuses drag/drop into FUSE mounts.
 */

/* popen/pclose live behind POSIX feature flags under -std=c11. */
#define _POSIX_C_SOURCE 200809L

#include "host_mount.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef __linux__
#include <sys/wait.h>
#endif

#define GUESTMOUNT_BIN     "/usr/bin/guestmount"
#define GUESTUNMOUNT_BIN   "/usr/bin/guestunmount"
#define UDISKSCTL_BIN      "/usr/bin/udisksctl"
#define GNOME_MOUNTER_BIN  "/usr/bin/gnome-disk-image-mounter"
#define XDG_OPEN_BIN       "/usr/bin/xdg-open"

#ifdef __linux__
static bool have_udisks(void) {
    static int cached = -1;
    if (cached < 0)
        cached = (access(UDISKSCTL_BIN, X_OK) == 0) ? 1 : 0;
    return cached == 1;
}

static bool have_guestmount(void) {
    static int cached = -1;
    if (cached < 0)
        cached = (access(GUESTMOUNT_BIN, X_OK) == 0 &&
                  access(GUESTUNMOUNT_BIN, X_OK) == 0) ? 1 : 0;
    return cached == 1;
}
#endif

bool host_mount_supported(void) {
#ifdef __linux__
    if (access(XDG_OPEN_BIN, X_OK) != 0) return false;
    return have_udisks() || have_guestmount();
#else
    return false;
#endif
}

#ifdef __linux__

/* Read all of `p` into `buf` (NUL-terminated, capped at sz-1). */
static void slurp(FILE *p, char *buf, size_t sz) {
    size_t n = fread(buf, 1, sz - 1, p);
    buf[n] = '\0';
}

/* Run `cmd` synchronously, capture stdout+stderr into out. Returns the
 * pclose exit status (0 on success). */
static int run_capture(const char *cmd, char *out, size_t sz) {
    out[0] = '\0';
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    slurp(p, out, sz);
    return pclose(p);
}

/* Scan `text` for the first match of `prefix`; copy the following
 * non-whitespace token into `dst`. Returns true if found. */
static bool extract_token_after(const char *text, const char *prefix,
                                char *dst, size_t sz) {
    const char *p = strstr(text, prefix);
    if (!p) return false;
    p += strlen(prefix);
    while (*p == ' ' || *p == '\t') p++;
    size_t i = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n' && i + 1 < sz)
        dst[i++] = *p++;
    dst[i] = '\0';
    /* Strip a trailing '.' that udisksctl prints. */
    if (i && dst[i - 1] == '.') dst[i - 1] = '\0';
    return dst[0] != '\0';
}

static void build_root(char *out, size_t sz) {
    /* Used only for the guestmount fallback. ~/.cache is a user-home path
     * with the right SELinux context (user_home_t), unlike /run/user
     * which Nautilus treats as foreign. */
    const char *home = getenv("HOME");
    if (!home || !home[0]) home = ".";
    snprintf(out, sz, "%s/.cache/1984/mounts", home);
}

static bool mkdir_p(const char *path) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
    return access(path, W_OK) == 0;
}

/* Resolve the loop device backing a given image file by parsing
 * `losetup -j <image>` output ("/dev/loopN: …"). Used after
 * gnome-disk-image-mounter, which doesn't tell us what it attached. */
static bool find_loop_for_image(const char *image, char *out, size_t sz) {
    char cmd[512], buf[1024];
    snprintf(cmd, sizeof(cmd), "losetup -j '%s' 2>/dev/null", image);
    if (run_capture(cmd, buf, sizeof(buf)) != 0) return false;
    char *colon = strchr(buf, ':');
    if (!colon || colon == buf) return false;
    size_t n = (size_t)(colon - buf);
    if (n >= sz) n = sz - 1;
    memcpy(out, buf, n);
    out[n] = '\0';
    return true;
}

/* Attach `image` as a loop device. Prefer gnome-disk-image-mounter (sets
 * up the loop, auto-mounts via udisks, integrates with GNOME Files —
 * user-confirmed "always works"); fall back to bare `udisksctl
 * loop-setup` for non-GNOME / minimal installs. On success writes
 * /dev/loopN into loop_dev_out. */
static bool udisks_loop_setup(const char *image, char *loop_dev_out, size_t sz) {
    char cmd[1024], out[1024];

    if (access(GNOME_MOUNTER_BIN, X_OK) == 0) {
        snprintf(cmd, sizeof(cmd),
                 "%s --writable '%s' 2>&1",
                 GNOME_MOUNTER_BIN, image);
        if (run_capture(cmd, out, sizeof(out)) == 0) {
            /* gnome-disk-image-mounter prints nothing useful — it just
             * spawns the udisks call and returns. Find the loop device
             * it attached by image path. */
            if (find_loop_for_image(image, loop_dev_out, sz))
                return true;
        }
        /* Fall through to bare udisksctl if the GNOME wrapper failed. */
    }

    snprintf(cmd, sizeof(cmd),
             "%s loop-setup --file='%s' --no-user-interaction 2>&1",
             UDISKSCTL_BIN, image);
    int rc = run_capture(cmd, out, sizeof(out));
    if (rc != 0) {
        fprintf(stderr, "host_mount: loop-setup failed for %s: %s",
                image, out);
        return false;
    }
    /* "Mapped file <image> as /dev/loopN." */
    return extract_token_after(out, "as ", loop_dev_out, sz);
}

/* Query the current mount point of a device via findmnt. Returns true if
 * the device is mounted somewhere. */
static bool findmnt_target(const char *dev, char *out, size_t sz) {
    char cmd[256], buf[512];
    snprintf(cmd, sizeof(cmd), "findmnt -n -o TARGET %s 2>/dev/null", dev);
    if (run_capture(cmd, buf, sizeof(buf)) != 0) return false;
    /* Strip trailing newline. */
    size_t n = strlen(buf);
    while (n && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
    if (!n) return false;
    snprintf(out, sz, "%s", buf);
    return true;
}

/* udisks mount of an attached loop device. Writes the resulting mount
 * point into mountpoint_out. gnome-disk-image-mounter often auto-mounts
 * before we get here, so we always check findmnt first. */
static bool udisks_mount(const char *loop_dev, char *mountpoint_out, size_t sz) {
    char cmd[512], out[1024];
    char part[80];
    snprintf(part, sizeof(part), "%sp1", loop_dev);

    /* If already mounted (auto-mount by gnome-disk-image-mounter), we're
     * done — just record where it ended up. */
    if (findmnt_target(part,     mountpoint_out, sz)) return true;
    if (findmnt_target(loop_dev, mountpoint_out, sz)) return true;

    /* Not auto-mounted yet — issue an explicit udisksctl mount. Try
     * partition first (most images), then bare device (no PT). */
    const char *targets[] = { part, loop_dev };
    for (size_t i = 0; i < sizeof(targets)/sizeof(targets[0]); i++) {
        snprintf(cmd, sizeof(cmd),
                 "%s mount -b %s --no-user-interaction 2>&1",
                 UDISKSCTL_BIN, targets[i]);
        int rc = run_capture(cmd, out, sizeof(out));
        if (rc == 0 && extract_token_after(out, " at ", mountpoint_out, sz))
            return true;
        /* Race: another process auto-mounted between our findmnt and now. */
        if (findmnt_target(targets[i], mountpoint_out, sz)) return true;
    }
    fprintf(stderr, "host_mount: udisksctl mount failed for %s: %s",
            loop_dev, out);
    return false;
}

/* Tear down a previously-attached loop. Order matters: unmount first,
 * then delete the loop device. udisks-based unmount also nukes the
 * auto-created /run/media/$USER/<label> directory. */
static void udisks_teardown(const char *loop_dev) {
    if (!loop_dev || !loop_dev[0]) return;
    char cmd[512], out[1024];
    /* Unmount partition (if present) and whole device. */
    char part[80];
    snprintf(part, sizeof(part), "%sp1", loop_dev);
    snprintf(cmd, sizeof(cmd),
             "%s unmount -b %s --no-user-interaction 2>&1",
             UDISKSCTL_BIN, part);
    (void)run_capture(cmd, out, sizeof(out));
    snprintf(cmd, sizeof(cmd),
             "%s unmount -b %s --no-user-interaction 2>&1",
             UDISKSCTL_BIN, loop_dev);
    (void)run_capture(cmd, out, sizeof(out));
    snprintf(cmd, sizeof(cmd),
             "%s loop-delete -b %s --no-user-interaction 2>&1",
             UDISKSCTL_BIN, loop_dev);
    (void)run_capture(cmd, out, sizeof(out));
}

/* guestmount fallback. Two axes of retry (backend + allow_other). */
static bool guestmount_image(const char *image, const char *path) {
    static const char *mount_specs[] = { "/dev/sda1", "/dev/sda" };
    static const char *backends[]    = { "", "LIBGUESTFS_BACKEND=direct " };
    unsigned uid = (unsigned)getuid();
    unsigned gid = (unsigned)getgid();
    char cmd[1024], errbuf[512];
    for (size_t b = 0; b < sizeof(backends)/sizeof(backends[0]); b++) {
        for (int try_allow_other = 0; try_allow_other <= 1; try_allow_other++) {
            for (size_t i = 0; i < sizeof(mount_specs)/sizeof(mount_specs[0]); i++) {
                snprintf(cmd, sizeof(cmd),
                         "%s%s -a '%s' -m %s --rw "
                         "-o uid=%u,gid=%u,umask=0%s '%s' 2>&1",
                         backends[b], GUESTMOUNT_BIN, image, mount_specs[i],
                         uid, gid,
                         try_allow_other ? ",allow_other" : "",
                         path);
                int rc = run_capture(cmd, errbuf, sizeof(errbuf));
                if (rc == 0) return true;
                if (errbuf[0])
                    fprintf(stderr, "host_mount: guestmount[%s%s,%s]: %s",
                            backends[b][0] ? "direct," : "default,",
                            mount_specs[i],
                            try_allow_other ? "allow_other" : "no-allow_other",
                            errbuf);
            }
        }
    }
    return false;
}

static void guestmount_unmount(const char *path) {
    char cmd[512], out[256];
    snprintf(cmd, sizeof(cmd), "%s '%s' 2>&1", GUESTUNMOUNT_BIN, path);
    (void)run_capture(cmd, out, sizeof(out));
}

/* Fork + exec xdg-open detached so the file manager runs independently. */
static void spawn_filemanager(const char *path) {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        execl(XDG_OPEN_BIN, "xdg-open", path, (char *)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, WNOHANG);
}

/* Append a slot to hm if `image` is a non-empty path to a readable file.
 * Doesn't mount — that happens in a second pass. */
static void stage_slot(HostMount *hm, const char *label, const char *image) {
    if (!image || image[0] == '\0') return;
    if (access(image, R_OK) != 0) {
        fprintf(stderr, "host_mount: %s image not readable: %s\n", label, image);
        return;
    }
    if (hm->count >= HOST_MOUNT_MAX_SLOTS) return;

    HostMountSlot *s = &hm->slots[hm->count];
    memset(s, 0, sizeof(*s));
    snprintf(s->label, sizeof(s->label), "%s", label);
    s->image = image;
    hm->count++;
}

bool host_mount_open(HostMount *hm, const Config *cfg) {
    memset(hm, 0, sizeof(*hm));

    /* Only mount cards whose hardware toggle is actually on. M4 directory
     * mode (cfg->m4_path) serves files from the host already — skip M4. */
    if (cfg->m4 && !cfg->m4_path[0])
        stage_slot(hm, "m4",      cfg->m4_image);
    if (cfg->symbiface_ide)
        stage_slot(hm, "ide",     cfg->ide_image);
    if (cfg->albireo)
        stage_slot(hm, "albireo", cfg->albireo_image);

    if (hm->count == 0) return false;

    int mounted = 0;
    char first_path[256] = "";

    for (int i = 0; i < hm->count; i++) {
        HostMountSlot *s = &hm->slots[i];

        /* Primary: udisksctl. Mounts under /run/media/$USER/<label>/ as
         * a first-class GNOME/KDE removable volume — Nautilus etc. treat
         * drag/drop natively. */
        if (have_udisks() && udisks_loop_setup(s->image, s->loop_dev,
                                                sizeof(s->loop_dev))) {
            if (udisks_mount(s->loop_dev, s->path, sizeof(s->path))) {
                s->via = HOST_MOUNT_UDISKS;
                mounted++;
                if (!first_path[0])
                    snprintf(first_path, sizeof(first_path), "%s", s->path);
                continue;
            }
            /* mount failed — tear down the loop we just set up. */
            udisks_teardown(s->loop_dev);
            s->loop_dev[0] = '\0';
        }

        /* Fallback: guestmount under ~/.cache/1984/mounts/<label>/ */
        if (have_guestmount()) {
            if (!hm->root[0]) {
                build_root(hm->root, sizeof(hm->root));
                mkdir_p(hm->root);
            }
            snprintf(s->path, sizeof(s->path), "%s/%s", hm->root, s->label);
            mkdir(s->path, 0700);
            if (guestmount_image(s->image, s->path)) {
                s->via = HOST_MOUNT_GUESTMOUNT;
                mounted++;
                if (!first_path[0])
                    snprintf(first_path, sizeof(first_path), "%s", s->path);
                continue;
            }
            rmdir(s->path);
            s->path[0] = '\0';
        }

        fprintf(stderr, "host_mount: no working mount backend for %s\n", s->image);
    }

    if (mounted == 0) {
        if (hm->root[0]) rmdir(hm->root);
        hm->count = 0;
        return false;
    }

    /* udisks mounts land in /run/media/$USER/<label>; opening the parent
     * shows every mounted card as a sibling. guestmount mounts live in
     * hm->root. If we have a mix, pick whichever path actually exists. */
    const char *open_target = NULL;
    char parent[256];
    if (mounted >= 1 && hm->slots[0].via == HOST_MOUNT_UDISKS) {
        const char *slash = strrchr(first_path, '/');
        if (slash && slash > first_path) {
            size_t n = (size_t)(slash - first_path);
            if (n < sizeof(parent)) {
                memcpy(parent, first_path, n);
                parent[n] = '\0';
                open_target = parent;
            }
        }
    }
    if (!open_target) open_target = hm->root[0] ? hm->root : first_path;

    spawn_filemanager(open_target);
    fprintf(stderr, "host_mount: mounted %d card(s); opened %s\n",
            mounted, open_target);
    return true;
}

bool host_mount_externally_unmounted(const HostMount *hm) {
    for (int i = 0; i < hm->count; i++) {
        const HostMountSlot *s = &hm->slots[i];
        if (!s->path[0]) continue;
        /* findmnt returns 0 only if the path is currently a mount point.
         * Nautilus eject removes the entry; we then know the user is done. */
        char cmd[512], buf[256];
        snprintf(cmd, sizeof(cmd),
                 "findmnt -n --target '%s' -o TARGET 2>/dev/null", s->path);
        if (run_capture(cmd, buf, sizeof(buf)) != 0) return true;
        size_t n = strlen(buf);
        while (n && (buf[n-1] == '\n' || buf[n-1] == ' ')) buf[--n] = '\0';
        if (strcmp(buf, s->path) != 0) return true;
    }
    return false;
}

bool host_mount_close(HostMount *hm) {
    for (int i = 0; i < hm->count; i++) {
        HostMountSlot *s = &hm->slots[i];
        if (s->via == HOST_MOUNT_UDISKS) {
            udisks_teardown(s->loop_dev);
        } else if (s->via == HOST_MOUNT_GUESTMOUNT && s->path[0]) {
            guestmount_unmount(s->path);
            rmdir(s->path);
        }
    }
    if (hm->root[0]) rmdir(hm->root);
    memset(hm, 0, sizeof(*hm));
    return true;
}

#else  /* !__linux__ */

bool host_mount_open(HostMount *hm, const Config *cfg) {
    (void)hm; (void)cfg;
    fprintf(stderr, "host_mount: not supported on this platform\n");
    return false;
}

bool host_mount_close(HostMount *hm) {
    (void)hm;
    return true;
}

bool host_mount_externally_unmounted(const HostMount *hm) {
    (void)hm;
    return false;
}

#endif

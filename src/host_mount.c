/* F10 guestmount feature — see issue #142.
 *
 * Pauses the emulator, calls libguestfs's `guestmount` on every active FAT
 * card image (M4 SD, IDE, Albireo USB), and opens the host file manager
 * at the mount root. The caller (main.c) is responsible for triggering a
 * cold reboot on close so the guest's stale FAT cache is discarded. */

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

#define GUESTMOUNT_BIN "/usr/bin/guestmount"
#define GUESTUNMOUNT_BIN "/usr/bin/guestunmount"
#define XDG_OPEN_BIN   "/usr/bin/xdg-open"

bool host_mount_supported(void) {
#ifdef __linux__
    static int cached = -1;
    if (cached < 0) {
        cached = (access(GUESTMOUNT_BIN, X_OK) == 0
               && access(GUESTUNMOUNT_BIN, X_OK) == 0
               && access(XDG_OPEN_BIN, X_OK) == 0) ? 1 : 0;
    }
    return cached == 1;
#else
    return false;
#endif
}

#ifdef __linux__

static void build_root(char *out, size_t sz) {
    snprintf(out, sz, "/run/user/%u/1984", (unsigned)getuid());
}

/* Try to mount `image` at `path` via guestmount. Returns true on success.
 *
 * libguestfs's `-i` inspector wants to identify a "guest OS" (Linux,
 * Windows, …); CPC card images don't have one, so we mount the FS
 * directly. Try /dev/sda1 first (partitioned images: M4 SymbOS SD, IDE
 * formatted by CP/M Plus, …), then fall back to /dev/sda (bare-FAT
 * images with no partition table — common for small USB sticks). */
static bool guestmount_image(const char *image, const char *path) {
    static const char *mount_specs[] = { "/dev/sda1", "/dev/sda" };
    char cmd[1024];
    /* -o uid/gid: by default guestmount maps the FUSE mount as root, so the
     * invoking user can't write. Pass our real uid/gid so dragging files
     * into the file manager works. FAT doesn't preserve groups, but the
     * permission check is on uid alone. */
    unsigned uid = (unsigned)getuid();
    unsigned gid = (unsigned)getgid();
    for (size_t i = 0; i < sizeof(mount_specs)/sizeof(mount_specs[0]); i++) {
        snprintf(cmd, sizeof(cmd),
                 "%s -a '%s' -m %s --rw -o uid=%u,gid=%u '%s' 2>/dev/null",
                 GUESTMOUNT_BIN, image, mount_specs[i], uid, gid, path);
        if (system(cmd) == 0)
            return true;
    }
    fprintf(stderr, "host_mount: guestmount failed for %s (tried -m /dev/sda1 and -m /dev/sda)\n",
            image);
    return false;
}

static void guestunmount_path(const char *path) {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "%s '%s' 2>&1", GUESTUNMOUNT_BIN, path);
    int rc = system(cmd);
    if (rc != 0)
        fprintf(stderr, "host_mount: guestunmount failed for %s (exit=%d)\n",
                path, rc);
}

/* Fork + exec xdg-open so the file manager runs detached from us. We
 * don't wait for it — the user closes it when they're done. */
static void spawn_filemanager(const char *path) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "host_mount: fork failed for xdg-open\n");
        return;
    }
    if (pid == 0) {
        /* Detach from controlling terminal so closing the emulator window
         * later doesn't reap the file manager too. */
        setsid();
        execl(XDG_OPEN_BIN, "xdg-open", path, (char *)NULL);
        _exit(127);   /* exec failed */
    }
    /* Parent: reap the immediate child (xdg-open is itself a launcher
     * that double-forks; we won't get a zombie). */
    int status;
    waitpid(pid, &status, WNOHANG);
}

/* Append a slot to hm if `image` is a non-empty path to a readable file.
 * Creates the mount-point directory. Does NOT mount yet — that happens in
 * a second pass so we can short-circuit cleanly when nothing is eligible. */
static void stage_slot(HostMount *hm, const char *label, const char *image) {
    if (!image || image[0] == '\0') return;
    if (access(image, R_OK) != 0) {
        fprintf(stderr, "host_mount: %s image not readable: %s\n", label, image);
        return;
    }
    if (hm->count >= HOST_MOUNT_MAX_SLOTS) return;

    HostMountSlot *s = &hm->slots[hm->count];
    snprintf(s->label, sizeof(s->label), "%s", label);
    snprintf(s->path,  sizeof(s->path),  "%s/%s", hm->root, label);
    s->image = image;
    hm->count++;
}

bool host_mount_open(HostMount *hm, const Config *cfg) {
    memset(hm, 0, sizeof(*hm));
    build_root(hm->root, sizeof(hm->root));

    /* M4 directory mode (cfg->m4_path) serves files from a host directory
     * already — no sector image to mount. Skip the M4 slot in that case. */
    if (!cfg->m4_path[0])
        stage_slot(hm, "m4",      cfg->m4_image);
    stage_slot(hm, "ide",     cfg->ide_image);
    stage_slot(hm, "albireo", cfg->albireo_image);

    if (hm->count == 0) return false;

    /* mkdir -p root */
    if (mkdir(hm->root, 0700) != 0 && access(hm->root, W_OK) != 0) {
        fprintf(stderr, "host_mount: cannot create %s\n", hm->root);
        hm->count = 0;
        return false;
    }

    int mounted = 0;
    for (int i = 0; i < hm->count; i++) {
        HostMountSlot *s = &hm->slots[i];
        mkdir(s->path, 0700);   /* may already exist from a prior abort */
        if (guestmount_image(s->image, s->path)) {
            mounted++;
        } else {
            /* Mark this slot as not actually mounted so close() skips it. */
            rmdir(s->path);
            s->path[0] = '\0';
        }
    }

    if (mounted == 0) {
        rmdir(hm->root);
        hm->count = 0;
        return false;
    }

    spawn_filemanager(hm->root);
    fprintf(stderr, "host_mount: mounted %d card(s) under %s\n",
            mounted, hm->root);
    return true;
}

bool host_mount_close(HostMount *hm) {
    for (int i = 0; i < hm->count; i++) {
        HostMountSlot *s = &hm->slots[i];
        if (s->path[0]) {
            guestunmount_path(s->path);
            rmdir(s->path);
        }
    }
    rmdir(hm->root);
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

#endif

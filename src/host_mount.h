#pragma once
#include <stdbool.h>
#include "config.h"

/* Which backend mounted this slot — determines how to unmount. */
typedef enum {
    HOST_MOUNT_NONE       = 0,
    HOST_MOUNT_UDISKS     = 1,   /* gnome-disk-image-mounter / udisksctl */
    HOST_MOUNT_GUESTMOUNT = 2    /* libguestfs FUSE fallback */
} HostMountVia;

/* Per-card mount slot. `path` is the directory the image is mounted onto;
 * empty when nothing is mounted. `image` is a borrowed pointer back into
 * the live Config so we know which file the mount is backed by. */
typedef struct {
    char         path[256];
    char         label[16];     /* "m4" | "ide" | "albireo" */
    char         loop_dev[64];  /* /dev/loopN — populated for HOST_MOUNT_UDISKS */
    const char  *image;
    HostMountVia via;
} HostMountSlot;

#define HOST_MOUNT_MAX_SLOTS 3

typedef struct {
    HostMountSlot slots[HOST_MOUNT_MAX_SLOTS];
    int           count;
    char          root[256];    /* used only for the guestmount fallback */
    bool          active;       /* F10 toggle state */
} HostMount;

/* True if the host has at least one working mount backend plus xdg-open. */
bool host_mount_supported(void);

/* Mount every active card image (M4 / IDE / Albireo) and open the host
 * file manager at the mount root. Returns true if at least one slot
 * mounted; false if nothing was eligible or every backend failed. */
bool host_mount_open(HostMount *hm, const Config *cfg);

/* Unmount every populated slot. Idempotent. */
bool host_mount_close(HostMount *hm);

/* Poll once per frame while the mount is active: return true if the user
 * ejected any card from the host file manager (Nautilus etc.) so the
 * caller can finish the close-and-cold-boot cycle automatically. */
bool host_mount_externally_unmounted(const HostMount *hm);

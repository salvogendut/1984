#pragma once
#include <stdbool.h>
#include "config.h"

/* Per-card mount slot. `path` is the directory the image is FUSE-mounted
 * onto; empty when nothing is mounted. `image` is a borrowed pointer back
 * into the live Config so we know which file to call guestmount on (and
 * which one to leave alone on close). */
typedef struct {
    char        path[256];
    char        label[16];      /* "m4" | "ide" | "albireo" */
    const char *image;
} HostMountSlot;

#define HOST_MOUNT_MAX_SLOTS 3

typedef struct {
    HostMountSlot slots[HOST_MOUNT_MAX_SLOTS];
    int           count;
    char          root[256];
    bool          active;       /* F10 toggle state */
} HostMount;

/* True if the host has the tools we need (guestmount + xdg-open) and we
 * built with support for this feature. Result cached internally — cheap to
 * call repeatedly. */
bool host_mount_supported(void);

/* Mount every active FAT card image under /run/user/$UID/1984/<label>/ and
 * fork+exec xdg-open on the root. Returns true if at least one slot
 * mounted (so the caller knows to enter the "browsing" state); false if
 * nothing was eligible or every guestmount call failed. On false the
 * struct is left clean (no partial mounts to clean up). */
bool host_mount_open(HostMount *hm, const Config *cfg);

/* fusermount-unmount every populated slot, rmdir the slot dirs and the
 * root. Idempotent: safe to call when nothing is mounted. */
bool host_mount_close(HostMount *hm);

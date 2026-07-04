#include "disk.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void test_sector_write_persists_after_reload(void) {
    const char *path = "/tmp/1984-test-disk-writeback.dsk";
    uint8_t pattern[512];

    for (int i = 0; i < (int)sizeof(pattern); i++)
        pattern[i] = (uint8_t)(i ^ 0x5A);

    unlink(path);
    assert(disk_create_blank(path) == 0);

    Disk disk;
    disk_init(&disk);
    assert(disk_load(&disk, path) == 0);
    assert(disk.inserted);
    assert(!disk.write_protected);

    DiskSector *sec = disk_find_sector(&disk, 0, 0, 0, 0xC1, 2);
    assert(sec);
    assert(sec->size == (int)sizeof(pattern));
    assert(disk_write_sector(&disk, sec, pattern, sec->size) == 0);
    disk_eject(&disk);

    disk_init(&disk);
    assert(disk_load(&disk, path) == 0);
    sec = disk_find_sector(&disk, 0, 0, 0, 0xC1, 2);
    assert(sec);

    DiskTrack *tr = &disk.track[disk.cur_track][0];
    assert(tr->data);
    assert(sec->offset + sec->size <= tr->data_size);
    assert(memcmp(tr->data + sec->offset, pattern, sizeof(pattern)) == 0);

    disk_eject(&disk);
    unlink(path);
}

int main(void) {
    test_sector_write_persists_after_reload();
    return 0;
}

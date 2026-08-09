#include "disk.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void test_new_disk_extension(void) {
    char plain[64] = "blank";
    char lower[64] = "blank.dsk";
    char upper[64] = "blank.DSK";
    char other[64] = "blank.img";
    char dotted_dir[64] = "/tmp/archive.v1/blank";
    char exact[9] = "disk";
    char too_small[8] = "disk";
    char empty[8] = "";

    assert(disk_ensure_dsk_extension(plain, sizeof(plain)));
    assert(strcmp(plain, "blank.dsk") == 0);

    assert(disk_ensure_dsk_extension(lower, sizeof(lower)));
    assert(strcmp(lower, "blank.dsk") == 0);

    assert(disk_ensure_dsk_extension(upper, sizeof(upper)));
    assert(strcmp(upper, "blank.DSK") == 0);

    assert(disk_ensure_dsk_extension(other, sizeof(other)));
    assert(strcmp(other, "blank.img.dsk") == 0);

    assert(disk_ensure_dsk_extension(dotted_dir, sizeof(dotted_dir)));
    assert(strcmp(dotted_dir, "/tmp/archive.v1/blank.dsk") == 0);

    assert(disk_ensure_dsk_extension(exact, sizeof(exact)));
    assert(strcmp(exact, "disk.dsk") == 0);

    assert(!disk_ensure_dsk_extension(too_small, sizeof(too_small)));
    assert(strcmp(too_small, "disk") == 0);

    assert(!disk_ensure_dsk_extension(empty, sizeof(empty)));
    assert(empty[0] == '\0');
    assert(!disk_ensure_dsk_extension(NULL, 0));
}

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

static void test_format_track_persists_after_reload(void) {
    const char *path = "/tmp/1984-test-disk-format.dsk";
    uint8_t ids[9 * 4];
    uint8_t pattern[512];

    for (int i = 0; i < 9; i++) {
        ids[i * 4 + 0] = 0;
        ids[i * 4 + 1] = 0;
        ids[i * 4 + 2] = (uint8_t)(0x41 + i);
        ids[i * 4 + 3] = 2;
    }
    memset(pattern, 0xA5, sizeof(pattern));

    unlink(path);
    assert(disk_create_blank(path) == 0);

    Disk disk;
    disk_init(&disk);
    assert(disk_load(&disk, path) == 0);

    DiskSector *sec = disk_find_sector(&disk, 0, 0, 0, 0xC1, 2);
    assert(sec);
    assert(disk_write_sector(&disk, sec, pattern, sec->size) == 0);
    assert(disk_format_track(&disk, 0, ids, 9, 2, 0x4E, 0xE5) == 0);
    disk_eject(&disk);

    disk_init(&disk);
    assert(disk_load(&disk, path) == 0);
    assert(disk_find_sector(&disk, 0, 0, 0, 0xC1, 2) == NULL);

    DiskTrack *tr = &disk.track[disk.cur_track][0];
    assert(tr->sector_count == 9);
    for (int i = 0; i < 9; i++) {
        sec = disk_find_sector(&disk, 0, 0, 0, (uint8_t)(0x41 + i), 2);
        assert(sec);
        assert(sec->offset + sec->size <= tr->data_size);
        for (int j = 0; j < sec->size; j++)
            assert(tr->data[sec->offset + j] == 0xE5);
    }

    disk_eject(&disk);
    unlink(path);
}

static void directory_entry(uint8_t *entry, uint8_t user,
                            const char name[8], const char ext[3],
                            uint8_t extent, uint8_t records) {
    memset(entry, 0, 32);
    entry[0] = user;
    memcpy(entry + 1, name, 8);
    memcpy(entry + 9, ext, 3);
    entry[12] = extent & 0x1F;
    entry[14] = extent >> 5;
    entry[15] = records;
}

static void write_directory(Disk *disk, int track, uint8_t first_sector,
                            const uint8_t directory[2048]) {
    disk->cur_track = track;
    for (int i = 0; i < 4; i++) {
        DiskSector *sec = disk_find_sector(
            disk, 0, (uint8_t)track, 0,
            (uint8_t)(first_sector + i), 2);
        assert(sec);
        assert(disk_write_sector(disk, sec, directory + i * 512, 512) == 0);
    }
}

static void test_list_data_directory(void) {
    const char *path = "/tmp/1984-test-disk-directory.dsk";
    uint8_t directory[2048];
    DiskDirectoryEntry files[DISK_DIRECTORY_MAX_FILES];

    memset(directory, 0xE5, sizeof(directory));
    directory_entry(directory, 0, "GAME    ", "BAS", 0, 128);
    directory_entry(directory + 32, 0, "GAME    ", "BAS", 1, 3);
    directory_entry(directory + 64, 1, "UTILITY ", "BI\316", 0, 10);
    directory_entry(directory + 96, 0x20, "LABEL   ", "   ", 0, 0);

    unlink(path);
    assert(disk_create_blank(path) == 0);

    Disk disk;
    disk_init(&disk);
    assert(disk_load(&disk, path) == 0);
    write_directory(&disk, 0, 0xC1, directory);
    disk_eject(&disk);
    assert(disk_load(&disk, path) == 0);

    int count = disk_list_directory(&disk, files, DISK_DIRECTORY_MAX_FILES);
    assert(count == 2);
    assert(files[0].user == 0);
    assert(strcmp(files[0].name, "GAME.BAS") == 0);
    assert(files[0].size == (128U + 3U) * 128U);
    assert(files[1].user == 1);
    assert(strcmp(files[1].name, "UTILITY.BIN") == 0);
    assert(files[1].size == 10U * 128U);

    disk_eject(&disk);
    unlink(path);
}

static void test_list_system_directory(void) {
    const char *path = "/tmp/1984-test-system-directory.dsk";
    uint8_t directory[2048];
    uint8_t ids[9 * 4];
    DiskDirectoryEntry files[DISK_DIRECTORY_MAX_FILES];

    memset(directory, 0xE5, sizeof(directory));
    directory_entry(directory, 0, "DISC    ", "   ", 0, 4);
    for (int i = 0; i < 9; i++) {
        ids[i * 4 + 0] = 0;
        ids[i * 4 + 1] = 0;
        ids[i * 4 + 2] = (uint8_t)(0x41 + i);
        ids[i * 4 + 3] = 2;
    }

    unlink(path);
    assert(disk_create_blank(path) == 0);

    Disk disk;
    disk_init(&disk);
    assert(disk_load(&disk, path) == 0);
    disk.cur_track = 0;
    assert(disk_format_track(&disk, 0, ids, 9, 2, 0x4E, 0xE5) == 0);
    for (int i = 0; i < 9; i++)
        ids[i * 4 + 0] = 2;
    disk.cur_track = 2;
    assert(disk_format_track(&disk, 0, ids, 9, 2, 0x4E, 0xE5) == 0);
    write_directory(&disk, 2, 0x41, directory);
    disk_eject(&disk);
    assert(disk_load(&disk, path) == 0);

    int count = disk_list_directory(&disk, files, DISK_DIRECTORY_MAX_FILES);
    assert(count == 1);
    assert(files[0].user == 0);
    assert(strcmp(files[0].name, "DISC") == 0);
    assert(files[0].size == 4U * 128U);

    disk_eject(&disk);
    unlink(path);
}

static void test_list_ibm_directory(void) {
    const char *path = "/tmp/1984-test-ibm-directory.dsk";
    uint8_t directory[2048];
    uint8_t ids[9 * 4];
    DiskDirectoryEntry files[DISK_DIRECTORY_MAX_FILES];

    memset(directory, 0xE5, sizeof(directory));
    directory_entry(directory, 0, "START   ", "BAS", 0, 2);
    for (int i = 0; i < 9; i++) {
        ids[i * 4 + 0] = 0;
        ids[i * 4 + 1] = 0;
        ids[i * 4 + 2] = (uint8_t)(0x01 + i);
        ids[i * 4 + 3] = 2;
    }

    unlink(path);
    assert(disk_create_blank(path) == 0);

    Disk disk;
    disk_init(&disk);
    assert(disk_load(&disk, path) == 0);
    disk.cur_track = 0;
    assert(disk_format_track(&disk, 0, ids, 9, 2, 0x4E, 0xE5) == 0);
    for (int i = 0; i < 9; i++)
        ids[i * 4 + 0] = 1;
    disk.cur_track = 1;
    assert(disk_format_track(&disk, 0, ids, 9, 2, 0x4E, 0xE5) == 0);
    write_directory(&disk, 1, 0x01, directory);
    disk_eject(&disk);
    assert(disk_load(&disk, path) == 0);

    int count = disk_list_directory(&disk, files, DISK_DIRECTORY_MAX_FILES);
    assert(count == 1);
    assert(files[0].user == 0);
    assert(strcmp(files[0].name, "START.BAS") == 0);
    assert(files[0].size == 2U * 128U);

    disk_eject(&disk);
    unlink(path);
}

int main(void) {
    test_new_disk_extension();
    test_sector_write_persists_after_reload();
    test_format_track_persists_after_reload();
    test_list_data_directory();
    test_list_system_directory();
    test_list_ibm_directory();
    return 0;
}

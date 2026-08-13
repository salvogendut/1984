#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "config.h"

static void cleanup_home(const char *home) {
    char path[CONFIG_PATH_MAX];
    snprintf(path, sizeof(path), "%s/.config/1984/1984.conf", home);
    unlink(path);
    snprintf(path, sizeof(path), "%s/.config/1984", home);
    rmdir(path);
    snprintf(path, sizeof(path), "%s/.config", home);
    rmdir(path);
    rmdir(home);
}

static void test_disk_autostart_round_trip(void) {
    const char *home = "/tmp/1984-test-config-home";
    const char *disk_a = "/tmp/My Game Disk.dsk";
    const char *disk_b = "/tmp/Utilities.dsk";
    cleanup_home(home);
    assert(mkdir(home, 0700) == 0);
    assert(setenv("HOME", home, 1) == 0);

    Config cfg;
    config_defaults(&cfg);
    assert(cfg.snapshot_breakpoints);
    cfg.snapshot_breakpoints = false;
    assert(config_disk_autostart_find(&cfg, disk_a) == NULL);
    assert(config_disk_autostart_set(&cfg, disk_a, 0, "OLD.BAS") == 1);
    assert(config_disk_autostart_set(&cfg, disk_a, 0, "game.bas") == 1);
    assert(config_disk_autostart_set(&cfg, disk_a, 0, "GAME.BAS") == 0);
    assert(config_disk_autostart_set(&cfg, disk_b, 3, "tools.bin") == 1);
    assert(config_disk_autostart_set(&cfg, disk_b, 16, "BAD.BIN") == -1);
    assert(config_disk_autostart_set(&cfg, disk_b, 0, "TOO-LONG9.BIN") == -1);
    assert(config_save(&cfg) == 0);

    Config loaded;
    assert(config_load(&loaded) == 0);
    assert(!loaded.snapshot_breakpoints);
    assert(loaded.disk_autostart_count == 2);
    const ConfigDiskAutostart *entry =
        config_disk_autostart_find(&loaded, disk_a);
    assert(entry);
    assert(entry->user == 0);
    assert(strcmp(entry->file, "GAME.BAS") == 0);
    entry = config_disk_autostart_find(&loaded, disk_b);
    assert(entry);
    assert(entry->user == 3);
    assert(strcmp(entry->file, "TOOLS.BIN") == 0);

    assert(config_disk_autostart_remove(&loaded, disk_a));
    assert(!config_disk_autostart_remove(&loaded, disk_a));
    assert(config_save(&loaded) == 0);

    Config reloaded;
    assert(config_load(&reloaded) == 0);
    assert(config_disk_autostart_find(&reloaded, disk_a) == NULL);
    entry = config_disk_autostart_find(&reloaded, disk_b);
    assert(entry && !strcmp(entry->file, "TOOLS.BIN"));
    cleanup_home(home);
}

int main(void) {
    test_disk_autostart_round_trip();
    puts("config tests passed");
    return 0;
}

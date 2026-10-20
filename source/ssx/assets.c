/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  assets.c
 * @brief Where APK asset paths live on the Vita.
 *
 * Assets come from DATA_PATH "assets/", as the data tool (datatool/) wrote
 * them: the APK's assets, some converted for the Vita in place, the folders
 * with thousands of files split into subfolders.
 */

#include "ssx/assets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <stdint.h>
#include <strings.h>

#include "utils/logger.h"

/*
 * Folders split into hashed subfolders by the data tool (datatool/ssxdata/layout.py), listed in
 * DATA_PATH "assets/_buckets.txt". The memory card's file system looks names
 * up by reading the folder's entries one by one: in worlds/geometry (4554
 * files) each open took ~420 ms, most of a race's two-minute load. A file of
 * a listed folder lives in its subfolder "_xx", xx being the FNV-1a hash of
 * its lowercase name modulo 64 (as in the tool).
 */
#define BUCKETS 64
#define MAX_BUCKET_DIRS 64

static char *bucket_dirs[MAX_BUCKET_DIRS];
static int nbucket_dirs;
static volatile int buckets_state; // 0: not loaded, 1: loading, 2: loaded

static void load_buckets(void) {
    int expected = 0;
    if (__atomic_compare_exchange_n(&buckets_state, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        SceUID fd = sceIoOpen(DATA_PATH "assets/_buckets.txt", SCE_O_RDONLY, 0);
        if (fd >= 0) {
            static char text[4096];
            int n = sceIoRead(fd, text, sizeof(text) - 1);
            sceIoClose(fd);
            text[n > 0 ? n : 0] = '\0';
            for (char *line = strtok(text, "\r\n"); line && nbucket_dirs < MAX_BUCKET_DIRS; line = strtok(NULL, "\r\n"))
                bucket_dirs[nbucket_dirs++] = line; // points into text, which stays
            l_info("Asset folders split into subfolders: %d", nbucket_dirs);
        }
        __atomic_store_n(&buckets_state, 2, __ATOMIC_RELEASE);
    }
    while (__atomic_load_n(&buckets_state, __ATOMIC_ACQUIRE) != 2)
        sceKernelDelayThread(100);
}

// If name's folder is split, writes its bucket subfolder ("_xx/") and returns 1
static int bucket_of(const char *name, char *sub) {
    load_buckets();
    const char *slash = strrchr(name, '/');
    if (!slash || !nbucket_dirs)
        return 0;
    size_t dir_len = slash - name + 1; // with the slash
    for (int i = 0; i < nbucket_dirs; i++) {
        if (strlen(bucket_dirs[i]) == dir_len && strncasecmp(bucket_dirs[i], name, dir_len) == 0) {
            uint32_t h = 0x811C9DC5;
            for (const char *p = slash + 1; *p; p++) {
                char c = *p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p;
                h = (h ^ (uint8_t)c) * 0x01000193;
            }
            snprintf(sub, 8, "_%02x/", (unsigned)(h % BUCKETS));
            return 1;
        }
    }
    return 0;
}

void ssx_asset_path(char *out, size_t size, const char *name) {
    while (*name == '/')
        name++;

    char sub[8];
    if (bucket_of(name, sub)) {
        const char *slash = strrchr(name, '/');
        snprintf(out, size, DATA_PATH "assets/%.*s%s%s", (int)(slash - name + 1), name, sub, slash + 1);
    } else {
        snprintf(out, size, DATA_PATH "assets/%s", name);
    }

    // Trailing slashes break sceIoDopen/sceIoGetstat
    size_t len = strlen(out);
    while (len > 0 && out[len - 1] == '/')
        out[--len] = '\0';
}

/*
 * The game is given Android-style paths for its writable directories, because
 * EA's rw::core::filesys parses "xxx:" as a device name and has no "ux0"
 * device. Every path-taking libc call maps them back here.
 */

static const struct {
    const char *android;
    const char *vita;
} path_map[] = {
    { ANDROID_APP_DIR "/", DATA_PATH },             // files/, cache/ ...
    { ANDROID_SDCARD "/", DATA_PATH "external/" },
    { ANDROID_SDCARD, DATA_PATH "external" },
    { "/data/", DATA_PATH "data/" },
    { "/data", DATA_PATH "data" },
};

const char *ssx_path(const char *path, char *buf, size_t size) {
    if (!path || path[0] != '/')
        return path;

    for (size_t i = 0; i < sizeof(path_map) / sizeof(path_map[0]); i++) {
        size_t n = strlen(path_map[i].android);
        int dir_prefix = path_map[i].android[n - 1] == '/';
        if (strncmp(path, path_map[i].android, n) == 0 && (dir_prefix || path[n] == '\0')) {
            snprintf(buf, size, "%s%s", path_map[i].vita, path + n);
            return buf;
        }
    }
    return path;
}

/*
 * On first boot GameCore::CopyBigFilesOutsideAppBundle() copies the files the
 * game streams (audio streams/banks and movies) out of the APK into the files
 * directory, ~750 MB. On the Vita they already are plain files, so reads of
 * those copies are served from the assets instead, and the copies are never
 * made (see patch.c). The small XMLs the game rewrites after copying are not
 * listed here and are copied as usual.
 */

#define FILES_DIR DATA_PATH "files/"

static const struct {
    const char *name;
    const char *asset_dir;
} big_files[] = {
    { "stream_1.mus", "mobile/android/audio/pathfinder/ssx_fe/" },
    { "stream_2.mus", "mobile/android/audio/pathfinder/ssx_fe/" },
    { "music_samples.sbs", "mobile/android/audio/music/" },
};

static const char *big_file_dir(const char *name) {
    for (size_t i = 0; i < sizeof(big_files) / sizeof(big_files[0]); i++)
        if (strcmp(name, big_files[i].name) == 0)
            return big_files[i].asset_dir;

    size_t len = strlen(name);
    if (len > 4 && strcmp(name + len - 4, ".mp4") == 0)
        return "common/mp4/";

    return NULL;
}

int ssx_is_big_file(const char *path) {
    const char *slash = strrchr(path, '/');
    return big_file_dir(slash ? slash + 1 : path) != NULL;
}

const char *ssx_redirect_big_file(const char *path, char *buf, size_t size) {
    if (strncmp(path, FILES_DIR, sizeof(FILES_DIR) - 1) != 0)
        return path;

    const char *name = path + sizeof(FILES_DIR) - 1;
    const char *dir = big_file_dir(name);
    if (!dir)
        return path;

    snprintf(buf, size, DATA_PATH "assets/%s%s", dir, name);
    return buf;
}

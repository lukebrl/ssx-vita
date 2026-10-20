/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef SSX_ASSETS_H
#define SSX_ASSETS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Maps an APK asset path ("common/config/memory.cfg") to its file on the Vita,
// DATA_PATH "assets/<name>", in its bucket subfolder if its folder was split.
void ssx_asset_path(char *out, size_t size, const char *name);

// Android paths given to the game for its writable directories
#define ANDROID_APP_DIR    "/data/data/com.ea.ssx"
#define ANDROID_FILES_DIR  ANDROID_APP_DIR "/files"
#define ANDROID_SDCARD     "/sdcard"
#define ANDROID_EXT_FILES  ANDROID_SDCARD "/Android/data/com.ea.ssx/files"

// Maps those Android paths to ux0:data/... ; returns `path` itself if it is
// not one of them, else `buf`
const char *ssx_path(const char *path, char *buf, size_t size);

// Whether a file name (or path) is one of the "big files" the game copies out
// of the APK on first boot
int ssx_is_big_file(const char *path);
// For reads: maps DATA_PATH "files/<big file>" to its location in the assets.
// Returns `path` itself for any other path, else `buf`.
const char *ssx_redirect_big_file(const char *path, char *buf, size_t size);

#ifdef __cplusplus
};
#endif

#endif // SSX_ASSETS_H

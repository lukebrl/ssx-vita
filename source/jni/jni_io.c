/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  jni_io.c
 * @brief android.content.res.AssetManager, java.io.InputStream and
 *        android.content.res.AssetFileDescriptor.
 *
 * EAIO and rwfilesystem read APK assets through these Java classes. On the
 * Vita the assets are a plain directory: DATA_PATH "assets/".
 */

#include "jni/jni_ssx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>

#include "utils/logger.h"
#include "ssx/assets.h"

#define JSTREAM_MAGIC 0x4A535452 // "JSTR"
#define JAFD_MAGIC    0x4A414644 // "JAFD"

typedef struct {
    uint32_t magic;
    SceUID fd;
} JStream;

typedef struct {
    uint32_t magic;
    int64_t length;
} JAssetFd;

static void asset_path(char *out, size_t size, jstring jname) {
    const char *name = jni->GetStringUTFChars(&jni, jname, NULL);
    ssx_asset_path(out, size, name);
    jni->ReleaseStringUTFChars(&jni, jname, (char *)name);
}

// InputStream open(String fileName)
jobject AssetManager_open(jmethodID id, va_list args) {
    char path[512];
    asset_path(path, sizeof(path), va_arg(args, jstring));

    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        l_debug("[AssetManager] open(%s): not found", path);
        fjni_throw(); // java.io.FileNotFoundException
        return NULL;
    }

    JStream *s = malloc(sizeof(JStream));
    s->magic = JSTREAM_MAGIC;
    s->fd = fd;
    l_debug("[AssetManager] open(%s): %p", path, s);
    return s;
}

// AssetFileDescriptor openFd(String fileName)
jobject AssetManager_openFd(jmethodID id, va_list args) {
    char path[512];
    asset_path(path, sizeof(path), va_arg(args, jstring));

    SceIoStat st;
    if (sceIoGetstat(path, &st) < 0 || SCE_S_ISDIR(st.st_mode)) {
        l_debug("[AssetManager] openFd(%s): not found", path);
        fjni_throw();
        return NULL;
    }

    JAssetFd *a = malloc(sizeof(JAssetFd));
    a->magic = JAFD_MAGIC;
    a->length = st.st_size;
    return a;
}

// String[] list(String path)
jobject AssetManager_list(jmethodID id, va_list args) {
    char path[512];
    asset_path(path, sizeof(path), va_arg(args, jstring));

    SceUID dir = sceIoDopen(path);
    if (dir < 0) {
        l_debug("[AssetManager] list(%s): not found", path);
        return jda_alloc(0, FIELD_TYPE_OBJECT);
    }

    int count = 0, cap = 64;
    jobject *names = malloc(cap * sizeof(jobject));
    SceIoDirent ent;
    while (sceIoDread(dir, &ent) > 0) {
        if (count == cap) {
            cap *= 2;
            names = realloc(names, cap * sizeof(jobject));
        }
        names[count++] = jstr(ent.d_name);
    }
    sceIoDclose(dir);

    JavaDynArray *arr = jda_alloc(count, FIELD_TYPE_OBJECT);
    memcpy(arr->array, names, count * sizeof(jobject));
    free(names);
    return arr;
}

static JStream *stream_from(jobject obj) {
    JStream *s = (JStream *)obj;
    return (s && s->magic == JSTREAM_MAGIC) ? s : NULL;
}

// int read(byte[] b, int off, int len)
jint InputStream_read(jmethodID id, va_list args) {
    JStream *s = stream_from(jni_current_object());
    JavaDynArray *buf = va_arg(args, JavaDynArray *);
    jint off = va_arg(args, jint);
    jint len = va_arg(args, jint);

    if (!s || !buf || off < 0 || len < 0 || off + len > buf->len)
        return -1;
    if (len == 0)
        return 0;

    int n = sceIoRead(s->fd, (char *)buf->array + off, len);
    return (n > 0) ? n : -1;
}

// long skip(long n)
jlong InputStream_skip(jmethodID id, va_list args) {
    JStream *s = stream_from(jni_current_object());
    jlong n = va_arg(args, jlong);
    if (!s || n <= 0)
        return 0;

    SceOff cur = sceIoLseek(s->fd, 0, SCE_SEEK_CUR);
    SceOff end = sceIoLseek(s->fd, 0, SCE_SEEK_END);
    SceOff to = cur + n > end ? end : cur + n;
    sceIoLseek(s->fd, to, SCE_SEEK_SET);
    return to - cur;
}

// void close() -- InputStream and AssetFileDescriptor
void InputStream_close(jmethodID id, va_list args) {
    jobject obj = jni_current_object();
    JStream *s = stream_from(obj);
    if (s) {
        sceIoClose(s->fd);
        s->magic = 0;
        free(s);
        return;
    }

    JAssetFd *a = (JAssetFd *)obj;
    if (a && a->magic == JAFD_MAGIC) {
        a->magic = 0;
        free(a);
    }
}

// long getLength()
jlong AssetFileDescriptor_getLength(jmethodID id, va_list args) {
    JAssetFd *a = (JAssetFd *)jni_current_object();
    return (a && a->magic == JAFD_MAGIC) ? a->length : -1;
}

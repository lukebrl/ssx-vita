/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  filecopy.c
 * @brief EA::IO::File::Copy in one pass over the files on disk.
 *
 * Autosaves copy the ghost save to a new name with File::Copy, on the logic
 * thread. The game copies 2 KB at a time through read()/write(); the ghost
 * file is 25 MB as the game sees it (reimpl/sparse_save.c stores it in a
 * much smaller file), so that is ~12800 reads and writes through the sparse
 * layer: a ~1 s freeze on the "dimmer" screen after races.
 *
 * Copying the file as stored on disk gives the same result (a sparse file
 * is recognised by its header wherever it is) with a few large reads and
 * writes.
 */

#include "ssx/filecopy.h"

#include <stdint.h>
#include <stdlib.h>

#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

#include "ssx/assets.h"
#include "utils/logger.h"

extern so_module so_mod;

#define OFF_File_Copy 0x11dac4 // EA::IO::File::Copy(char const*, char const*, bool) [.part.125]: (src, dst)
#define CHUNK (32 * 1024)

typedef int (*CopyFn)(const char *, const char *);
static CopyFn Copy_orig;
static uint8_t buf[CHUNK]; // static: a malloc of it can fail when memory is short (it did, the copy was left empty)
static SceKernelLwMutexWork lock;

// The raw copy; 0 and a reason on failure
static int raw_copy(const char *s, const char *d, uint32_t *total, const char **why, int *err) {
    SceUID in = sceIoOpen(s, SCE_O_RDONLY, 0);
    if (in < 0) {
        *why = "can't open the source";
        *err = in;
        return 0;
    }
    SceUID out = sceIoOpen(d, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (out < 0) {
        sceIoClose(in);
        *why = "can't create the copy";
        *err = out;
        return 0;
    }
    int ok = 1;
    while (1) {
        int n = sceIoRead(in, buf, CHUNK);
        if (n < 0) {
            ok = 0;
            *why = "read failed";
            *err = n;
        }
        if (n <= 0)
            break;
        int w = sceIoWrite(out, buf, n);
        if (w != n) {
            ok = 0;
            *why = "write failed";
            *err = w;
            break;
        }
        *total += n;
    }
    sceIoClose(in);
    int c = sceIoClose(out);
    if (ok && c < 0) {
        ok = 0;
        *why = "closing the copy failed";
        *err = c;
    }
    return ok;
}

static int File_Copy(const char *src, const char *dst) {
    uint32_t t0 = sceKernelGetProcessTimeLow();
    char src_buf[512], dst_buf[512], redirect[512];
    const char *s = ssx_path(src, src_buf, sizeof(src_buf));
    s = ssx_redirect_big_file(s, redirect, sizeof(redirect));
    const char *d = ssx_path(dst, dst_buf, sizeof(dst_buf));

    uint32_t total = 0;
    const char *why = "";
    int err = 0;
    sceKernelLockLwMutex(&lock, 1, NULL);
    int ok = raw_copy(s, d, &total, &why, &err);
    sceKernelUnlockLwMutex(&lock, 1);
    if (ok) {
        l_info("[COPY] %s -> %s: %u KB on disk in %u ms", s, d, total / 1024,
               (sceKernelGetProcessTimeLow() - t0) / 1000);
        return 1;
    }
    // The game's own copy then: a save must not be lost to a failed shortcut
    // (the game deletes the old ghost files after the copy)
    sceIoRemove(d);
    ok = Copy_orig(src, dst);
    l_error("[COPY] %s -> %s: %s (0x%08X); the game's own copy %s", s, d, why, err, ok ? "worked" : "failed too");
    return ok;
}

void ssx_filecopy_patch(void) {
    sceKernelCreateLwMutex(&lock, "filecopy", 0, 0, NULL);
    Copy_orig = (CopyFn)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_File_Copy, (uintptr_t)File_Copy);
    if (!Copy_orig)
        l_error("[COPY] could not hook EA::IO::File::Copy");
}

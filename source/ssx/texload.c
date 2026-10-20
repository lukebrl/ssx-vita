/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  texload.c
 * @brief Reads texture files without holding the render thread's GPU lock.
 *
 * EA::CoreRender::TextureAsset::Translate, on the asset translator thread,
 * takes GameRender::AcquireThread (the lock that owns the GL context) and
 * then calls TranslatePvr or TranslateDds, which read the whole file from
 * the asset stream (blocking on the memory card) and upload it. The render
 * thread takes the same lock every frame (FSGameRenderFE::Process), so while
 * a UI screen's textures load it waits out the reads: 1-4 MB, 100-370 ms
 * per frame.
 *
 * Both functions are replaced: the file is read with the lock released, and
 * the lock is taken again only for the upload.
 */

#include "ssx/texload.h"

#include <stdint.h>
#include <string.h>

#include <kubridge.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>
#include <vitaGL.h>

#include "utils/logger.h"

extern so_module so_mod;

// libgame.so v0.0.8430
#define OFF_TranslateDds 0x5ba300     // EA::CoreRender::TranslateDds(TexturePlatC*, Stream&, ICoreAllocator*, DeviceC*, char const*)
#define OFF_TranslatePvr 0x5ba3e8     // EA::CoreRender::TranslatePvr(same)
#define OFF_LoadTextureFromDDS 0xd860c0 // RNAX::LoadTextureFromDDS(DeviceC*, TextureC*, void*, unsigned, char const*, ...)
#define OFF_LoadTextureFromPVR 0xd861e0 // RNAX::LoadTextureFromPVR(DeviceC*, TextureC*, void*, unsigned, char const*)
#define OFF_AcquireThread 0xdd766c    // GameRender::AcquireThread()
#define OFF_ReleaseThread 0xdd7778    // GameRender::ReleaseThread()
#define OFF_GameRender_ptr 0x1778bd8  // GOT entry of the GameRender* the translator locks

#define VT(o) (*(void ***)(o))
#define UPLOAD_SLOW_US 8000 // debug: uploads logged from this long
// AssetStream::Stream: GetSize() slot 3, Read(buf, size, 0) slot 5
// EA::Allocator::ICoreAllocator: Alloc(size, name, flags, align, offset) slot 3, Free(p, size) slot 4

typedef uint32_t (*StreamSizeFn)(void *stream);
typedef uint32_t (*StreamReadFn)(void *stream, void *buf, uint32_t size, uint32_t zero);
typedef void *(*AllocFn)(void *alloc, uint32_t size, const char *name, uint32_t flags, uint32_t align, uint32_t offset);
typedef void (*FreeFn)(void *alloc, void *p, uint32_t size);
typedef int (*LoadPvrFn)(void *device, void *tex, void *buf, uint32_t size, const char *name);
typedef int (*LoadDdsFn)(void *device, void *tex, void *buf, uint32_t size, const char *name, uint32_t zero);

static LoadPvrFn LoadTextureFromPVR;
static LoadDdsFn LoadTextureFromDDS;
static void (*AcquireThread)(void *gr);
static void (*ReleaseThread)(void *gr);
static uintptr_t gamerender_got; // GOT entry: the address of the GameRender* (relocated at load)

static volatile uint32_t loads, unlocked_kb;

// Mip levels in a texture file's header (0: unknown). vitaGL then allocates
// the whole chain at the first level: grown level by level it was
// reallocated each time, and moved levels were copied back by the CPU from
// GPU memory, ~15-20 ms per texture whatever its size ([UPLOAD]).
static uint32_t header_mips(const uint8_t *buf, uint32_t size, int dds) {
    uint32_t v = 0;
    if (dds) {
        if (size >= 32 && memcmp(buf, "DDS ", 4) == 0)
            memcpy(&v, buf + 28, 4); // dwMipMapCount (0: no mips)
        return v ? v : 1;
    }
    if (size >= 52) {
        uint32_t magic;
        memcpy(&magic, buf, 4);
        if (magic == 0x03525650) { // PVR v3: mip count (with the base level) at 44
            memcpy(&v, buf + 44, 4);
            return v;
        }
        memcpy(&magic, buf + 44, 4);
        if (magic == 0x21525650) { // PVR v2 ("PVR!"): mip count (without the base level) at 12
            memcpy(&v, buf + 12, 4);
            return v + 1;
        }
    }
    return 0;
}

static int translate(int dds, void *tex, void *stream, void *alloc, void *device, const char *name) {
    uint32_t size = ((StreamSizeFn)VT(stream)[3])(stream);
    void *buf = ((AllocFn)VT(alloc)[3])(alloc, size, NULL, 0, 16, 0);
    if (!buf)
        return 0;
    void *gr = **(void ***)gamerender_got;
    // Translate took the lock just before calling us: let the render thread
    // have it while the file is read
    ReleaseThread(gr);
    uint32_t n = ((StreamReadFn)VT(stream)[5])(stream, buf, size, 0);
    AcquireThread(gr);
    int r = 0;
#ifdef DEBUG_SOLOADER
    uint32_t t0 = sceKernelGetProcessTimeLow();
    uint32_t realloc0 = vgl_ssx_upload_realloc_us, fill0 = vgl_ssx_upload_fill_us;
#endif
    uint32_t mips = n == size ? header_mips(buf, size, dds) : 0;
    if (n == size) {
        vglSsxHintMipLevels(mips);
        r = dds ? LoadTextureFromDDS(device, tex, buf, size, name, 0) : LoadTextureFromPVR(device, tex, buf, size, name);
        vglSsxHintMipLevels(0);
    }
#ifdef DEBUG_SOLOADER
    // What holds the GPU lock (the render thread waits meanwhile): the upload
    uint32_t us = sceKernelGetProcessTimeLow() - t0;
    if (SSX_DIAG && us > UPLOAD_SLOW_US)
        l_info("[UPLOAD] texture %s: %u KB, %u mip levels, uploaded in %u ms under the GPU lock (vitaGL: chain "
               "allocation %u ms, levels %u ms)",
               name ? name : "?", (unsigned)(size / 1024), (unsigned)mips, (unsigned)(us / 1000),
               (unsigned)((vgl_ssx_upload_realloc_us - realloc0) / 1000), (unsigned)((vgl_ssx_upload_fill_us - fill0) / 1000));
#endif
    ((FreeFn)VT(alloc)[4])(alloc, buf, 0);
    loads++;
    unlocked_kb += size / 1024;
    return r;
}

static int TranslateDds_hook(void *tex, void *stream, void *alloc, void *device, const char *name) {
    return translate(1, tex, stream, alloc, device, name);
}
static int TranslatePvr_hook(void *tex, void *stream, void *alloc, void *device, const char *name) {
    return translate(0, tex, stream, alloc, device, name);
}

void ssx_texload_patch(void) {
    uintptr_t base = so_mod.text_base;
    LoadTextureFromPVR = (LoadPvrFn)(base + OFF_LoadTextureFromPVR);
    LoadTextureFromDDS = (LoadDdsFn)(base + OFF_LoadTextureFromDDS);
    AcquireThread = (void *)(base + OFF_AcquireThread);
    ReleaseThread = (void *)(base + OFF_ReleaseThread);
    gamerender_got = base + OFF_GameRender_ptr;

    // Both start "push {r4-r10, lr}; mov r5, r1"
    static const uint32_t funcs[] = { OFF_TranslateDds, OFF_TranslatePvr };
    static void *const hooks[] = { TranslateDds_hook, TranslatePvr_hook };
    for (int i = 0; i < 2; i++) {
        const uint32_t *code = (const uint32_t *)(base + funcs[i]);
        if (code[0] != 0xE92D47F0 || code[1] != 0xE1A05001) {
            l_error("[TEXLOAD] unexpected code at 0x%x (0x%08X 0x%08X), left as is", funcs[i], code[0], code[1]);
            continue;
        }
        hook_arm(base + funcs[i], (uintptr_t)hooks[i]);
        kuKernelFlushCaches((void *)(base + funcs[i]), 8);
    }
    l_info("[TEXLOAD] texture files are read without the GPU lock");
}

void ssx_texload_report(void) {
    if (loads)
        l_info("[PERF] textures loaded without the GPU lock: %u (%u KB)", loads, unlocked_kb);
    loads = unlocked_kb = 0;
}

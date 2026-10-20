/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  globeload.c
 * @brief The globe loads a mountain range's assets without pausing between them.
 *
 * When the globe moves to another range (a race unlocked in the campaign, a
 * new range picked), GlobeUiRangeCache loads that range's assets and the
 * camera waits for them, before and after the flight. GlobeUiRangeCache::
 * Process starts the next load only while at most one asset is pending in
 * the asset stream, and only once it has waited 0.15 s for that: about two
 * assets every 0.15 s (12-13 a second in the logs). A range is dozens of
 * assets, so the game sat still for seconds with the frame rate steady.
 *
 * The pause goes (threshold 0 s) and up to 4 assets may be pending: the
 * stream's load, unpack and translate threads then always have work.
 *
 * Debug builds with SSX_DIAG log how long each range took and the globe's
 * timeline ([GLOBE]), and slow model uploads ([UPLOAD]).
 */

#include "ssx/globeload.h"

#include <stdint.h>
#include <string.h>

#include <kubridge.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

// libgame.so v0.0.8430, in GlobeUiRangeCache::Process(float)
#define OFF_PendingCmp 0x2c7990   // cmp r0, #1: AssetStream::GetNumberAssetsPending(0) > 1 waits
#define OFF_DelayLiteral 0x2c79e8 // 0.15f: seconds with at most one pending before the next load
#define CMP_R0_1 0xe3500001
#define CMP_R0_MAX 0xe3500003     // cmp r0, #3: waits only above 3 pending (up to 4 in flight)
#define DELAY_015 0x3e19999a
#define DELAY_0 0x00000000

static int patch_word(uint32_t off, uint32_t expected, uint32_t value) {
    uint32_t *p = (uint32_t *)(so_mod.text_base + off);
    if (*p != expected) {
        l_error("[GLOBE] unexpected word at game+0x%X: 0x%08X", (unsigned)off, (unsigned)*p);
        return 0;
    }
    kuKernelCpuUnrestrictedMemcpy(p, &value, sizeof(value));
    kuKernelFlushCaches(p, sizeof(value));
    return 1;
}

#ifdef DEBUG_SOLOADER
#define OFF_RangeCache_Process 0x2c7920  // GlobeUiRangeCache::Process(float)
#define OFF_RangeCache_IsActive 0x2c6744 // GlobeUiRangeCache::IsActive() const: assets still to load

static void (*Process_orig)(void *, float);
static int (*IsActive)(void *);

// Models: GeometryAsset::TranslateHelper parses each one and makes its
// buffers under the GPU lock (GeomData::LoadFromStream), the render thread
// waiting meanwhile
#define OFF_GeomData_LoadFromStream 0x5b8d20 // EA::CoreRender::GeomData::LoadFromStream(InStreamC&, DeviceC*, bool)
#define UPLOAD_SLOW_US 8000
static int (*LoadFromStream_orig)(void *, void *, void *, int);

static int LoadFromStream_hook(void *geom, void *stream, void *device, int flag) {
    uint32_t t0 = sceKernelGetProcessTimeLow();
    int r = LoadFromStream_orig(geom, stream, device, flag);
    uint32_t us = sceKernelGetProcessTimeLow() - t0;
    if (us > UPLOAD_SLOW_US)
        l_info("[UPLOAD] model: parsed and uploaded in %u ms under the GPU lock", (unsigned)(us / 1000));
    return r;
}

// The globe's timeline ([GLOBE] held / moving): GlobeUiEntityManager::Process
// returns early, before the transition and the camera (GlobeController::
// Process), while its flag at +344 is set and ProcessAssetsLoad says the
// entity assets are not all loaded: the globe is held still.
#define OFF_EntityMgr_Process 0x2b1ee0       // GlobeUiEntityManager::Process(float)
#define OFF_EntityMgr_AssetsLoad 0x2b0948    // GlobeUiEntityManager::ProcessAssetsLoad(): 0 while loading
#define OFF_EntityMgr_IsTransitioning 0x2b1ea8
#define ENTITY_LOADING_FLAG(m) (*(volatile uint8_t *)((uint8_t *)(m) + 344))
#define ENTITY_CONTROLLER(m) (*(uint8_t **)((uint8_t *)(m) + 248))
#define CONTROLLER_CAMERA(c) (*(uint8_t **)((c) + 20))
#define CAMERA_DURATION(c) (*(volatile float *)((c) + 912)) // > 0 while a camera transition runs
#define CAMERA_ELAPSED(c) (*(volatile float *)((c) + 916))

static void (*EntityProcess_orig)(void *, float);
static int (*AssetsLoad_orig)(void *);
static int (*EntityIsTransitioning)(void *);
static int (*PendingAssets)(unsigned);
static int last_assets_done = 1;

static int AssetsLoad_hook(void *mgr) {
    return last_assets_done = AssetsLoad_orig(mgr);
}

static void EntityProcess_hook(void *mgr, float dt) {
    static int held, moving, transitioning;
    static uint32_t held_t0, move_t0, trans_t0;
    EntityProcess_orig(mgr, dt);
    uint32_t now = sceKernelGetProcessTimeLow();
    int h = ENTITY_LOADING_FLAG(mgr) && !last_assets_done;
    if (h != held) {
        if (h)
            l_info("[GLOBE] held for entity assets (%d pending in the asset stream)", PendingAssets(0));
        else
            l_info("[GLOBE] released after %u ms", (unsigned)((now - held_t0) / 1000));
        held = h;
        held_t0 = now;
    }
    uint8_t *ctl = ENTITY_CONTROLLER(mgr);
    uint8_t *cam = ctl ? CONTROLLER_CAMERA(ctl) : NULL;
    int m = cam && CAMERA_DURATION(cam) > 0.0f;
    if (m != moving) {
        if (m)
            l_info("[GLOBE] camera transition starts (%.2f s long)", CAMERA_DURATION(cam));
        else
            l_info("[GLOBE] camera transition done after %u ms", (unsigned)((now - move_t0) / 1000));
        moving = m;
        move_t0 = now;
    }
    int t = EntityIsTransitioning(mgr);
    if (t != transitioning) {
        if (t)
            l_info("[GLOBE] transition requested");
        else
            l_info("[GLOBE] transition over after %u ms", (unsigned)((now - trans_t0) / 1000));
        transitioning = t;
        trans_t0 = now;
    }
}

static void Process_hook(void *cache, float dt) {
    static void *loading;
    static uint32_t t0;
    if (!loading && IsActive(cache)) {
        loading = cache;
        t0 = sceKernelGetProcessTimeLow();
    }
    Process_orig(cache, dt);
    if (loading == cache && !IsActive(cache)) {
        l_info("[GLOBE] range assets loaded in %u ms", (unsigned)((sceKernelGetProcessTimeLow() - t0) / 1000));
        loading = NULL;
    }
}
#endif

void ssx_globeload_patch(void) {
    if (patch_word(OFF_PendingCmp, CMP_R0_1, CMP_R0_MAX) && patch_word(OFF_DelayLiteral, DELAY_015, DELAY_0))
        l_info("[GLOBE] range loading: up to 4 assets pending, no pause");
#ifdef DEBUG_SOLOADER
    if (!SSX_DIAG)
        return;
    IsActive = (void *)(so_mod.text_base + OFF_RangeCache_IsActive);
    Process_orig = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_RangeCache_Process, (uintptr_t)Process_hook);
    if (!Process_orig)
        l_error("[GLOBE] could not hook GlobeUiRangeCache::Process");
    EntityIsTransitioning = (void *)(so_mod.text_base + OFF_EntityMgr_IsTransitioning);
    PendingAssets = (void *)(so_mod.text_base + 0x665ec8); // AssetStream::GetNumberAssetsPending(unsigned)
    AssetsLoad_orig = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_EntityMgr_AssetsLoad, (uintptr_t)AssetsLoad_hook);
    EntityProcess_orig =
        (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_EntityMgr_Process, (uintptr_t)EntityProcess_hook);
    if (!AssetsLoad_orig || !EntityProcess_orig)
        l_error("[GLOBE] could not hook the entity manager");
    LoadFromStream_orig =
        (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_GeomData_LoadFromStream, (uintptr_t)LoadFromStream_hook);
    if (!LoadFromStream_orig)
        l_error("[GLOBE] could not hook GeomData::LoadFromStream");
#endif
}

/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  assetunload.c
 * @brief Unloads cached assets under one GPU lock instead of one per file.
 *
 * CramFileCache keeps the files of each asset class it loaded, with a
 * reference count per class. The files of a race's drop (its track, sky,
 * characters) stay loaded after the race, so the same race starts again
 * without loading; FEGameDataHandler::LoadDrop releases the previous drop's
 * class when the next race is picked on the character screen.
 * ReleaseAssetClass then unloads the class's files one by one, and each
 * FileRef::Unload takes GameRender::AcquireThread, the lock that owns the GL
 * context, around one AssetStream::Asset::Release.
 *
 * The render thread holds that lock while it draws a frame. At 30 Hz it is
 * free most of the time (a menu frame takes a few ms, then the loop sleeps);
 * at 60 Hz frames run back to back, and each FileRef::Unload waits for the
 * end of one (~16 ms). A race has about a thousand files: the character
 * screen took 6-19 s to come up after a race.
 *
 * GlobeUiRangeCache::Unload does the same with the globe menu's files when
 * a race loads: 173 waits, 2.8 s of the race's loading screen.
 *
 * Both now take the lock once, when files are about to be unloaded, and the
 * game's per-file AcquireThread calls only count up its recursion. The locks
 * are taken in the game's order (the cache's futex, then the GPU lock), and
 * the GPU lock only when the original would take it.
 *
 * Debug builds log the waits for the GPU lock ([GPUWAIT], every 5 s when
 * there were any): how many, how long, and the callers that waited longest.
 */

#include "ssx/assetunload.h"

#include <stdint.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

// libgame.so v0.0.8430
#define OFF_ReleaseAssetClass 0xf91da4 // CramFileCache::ReleaseAssetClass(Attrib::Key<1> const&)
#define OFF_RangeCache_Unload 0x2c6a34 // GlobeUiRangeCache::Unload()
#define OFF_RBTreeIncrement 0x4b0cec   // eastl::RBTreeIncrement(rbtree_node_base const*)
#define OFF_AcquireThread 0xdd766c     // GameRender::AcquireThread()
#define OFF_ReleaseThread 0xdd7778     // GameRender::ReleaseThread()
#define OFF_FutexLock 0x70d7e0         // EA::Thread::Futex::Lock() (recursive)
#define OFF_FutexUnlock 0x1453b4       // EA::Thread::Futex::Unlock()
#define OFF_GameRender_ptr 0x1778bd8   // GOT entry of the GameRender* FileRef::Unload locks

// CramFileCache: its futex at +20, a vector of AssetFileCache (36 bytes) at
// +4..+8. AssetFileCache: the class key at +4, its FileRefs at +12..+16,
// the reference count at +28.
#define CACHE_FUTEX(c) ((uint8_t *)(c) + 20)
#define CACHE_BEGIN(c) (*(uint8_t **)((uint8_t *)(c) + 4))
#define CACHE_END(c) (*(uint8_t **)((uint8_t *)(c) + 8))
#define ENTRY_SIZE 36
#define ENTRY_CLASS(e) (*(uint32_t *)((e) + 4))
#define ENTRY_FILES_BEGIN(e) (*(uint32_t *)((e) + 12))
#define ENTRY_FILES_END(e) (*(uint32_t *)((e) + 16))
#define ENTRY_REFS(e) (*(int32_t *)((e) + 28))
// GlobeUiRangeCache: a map of RangeAsset, its anchor node at +16, the first
// node at +20. A node's FileRefs (12 bytes each) at +20, their count at +36.
#define RANGE_ANCHOR(r) ((uint8_t *)(r) + 16)
#define RANGE_FIRST(r) (*(uint8_t **)((uint8_t *)(r) + 20))
#define NODE_FILES(n) (*(uint32_t *)((n) + 36))

static void (*ReleaseAssetClass_orig)(void *cache, const uint32_t *cls);
static void (*RangeCache_Unload_orig)(void *range);
static uint8_t *(*RBTreeIncrement)(const uint8_t *node);
static void (*AcquireThread)(void *gr);
static void (*ReleaseThread)(void *gr);
static void (*FutexLock)(void *f);
static void (*FutexUnlock)(void *f);
static uintptr_t gamerender_got; // GOT entry: the address of the GameRender* (relocated at load)

// Whether the call unloads files: a matching class whose last reference goes
static int unloads_files(void *cache, uint32_t cls) {
    for (uint8_t *e = CACHE_BEGIN(cache); e != CACHE_END(cache); e += ENTRY_SIZE)
        if (ENTRY_CLASS(e) == cls && ENTRY_REFS(e) == 1 && ENTRY_FILES_BEGIN(e) != ENTRY_FILES_END(e))
            return 1;
    return 0;
}

static void ReleaseAssetClass_hook(void *cache, const uint32_t *cls) {
    FutexLock(CACHE_FUTEX(cache));
    void *gr = unloads_files(cache, *cls) ? **(void ***)gamerender_got : NULL;
    if (gr)
        AcquireThread(gr);
    ReleaseAssetClass_orig(cache, cls);
    if (gr)
        ReleaseThread(gr);
    FutexUnlock(CACHE_FUTEX(cache));
}

static void RangeCache_Unload_hook(void *range) {
    int files = 0;
    for (uint8_t *n = RANGE_FIRST(range); n != RANGE_ANCHOR(range) && !files; n = RBTreeIncrement(n))
        files = NODE_FILES(n) != 0;
    void *gr = files ? **(void ***)gamerender_got : NULL;
    if (gr)
        AcquireThread(gr);
    RangeCache_Unload_orig(range);
    if (gr)
        ReleaseThread(gr);
}

#ifdef DEBUG_SOLOADER
#define OFF_GetThreadId 0x4a9118 // EA::Thread::GetThreadId()
#define REPORT_MS 5000
#define CALLERS 8
#define PORT_CALLER 0xFFFFFFFF

static void *(*AcquireThread_orig)(void *gr);
static uintptr_t (*GetThreadId)(void);
static struct {
    uint32_t caller, waits, us;
} callers[CALLERS];
static uint32_t waits, wait_us;
static SceUID stats_lock;

static void *AcquireThread_hook(void *gr) {
    // GameRender's futex: the count at +56, the owner at +64
    int32_t count = *(volatile int32_t *)((uint8_t *)gr + 56);
    uintptr_t owner = *(volatile uintptr_t *)((uint8_t *)gr + 64);
    if (count <= 0 || owner == GetThreadId())
        return AcquireThread_orig(gr);
    uint32_t caller = (uint32_t)__builtin_return_address(0) - so_mod.text_base;
    if (caller >= so_mod.text_size)
        caller = PORT_CALLER; // the port's own code (texload.c)
    uint32_t t0 = sceKernelGetProcessTimeLow();
    void *r = AcquireThread_orig(gr);
    uint32_t us = sceKernelGetProcessTimeLow() - t0;
    sceKernelLockMutex(stats_lock, 1, NULL);
    waits++;
    wait_us += us;
    int slot = -1;
    for (int i = 0; i < CALLERS && slot < 0; i++)
        if (callers[i].caller == caller || !callers[i].caller)
            slot = i;
    if (slot >= 0) {
        callers[slot].caller = caller;
        callers[slot].waits++;
        callers[slot].us += us;
    }
    sceKernelUnlockMutex(stats_lock, 1);
    return r;
}

static int report_thread(SceSize args, void *argp) {
    while (1) {
        sceKernelDelayThread(REPORT_MS * 1000);
        char line[256];
        int len = 0;
        sceKernelLockMutex(stats_lock, 1, NULL);
        uint32_t n = waits, us = wait_us;
        for (int k = 0; k < 3; k++) { // the three callers that waited longest
            int best = -1;
            for (int i = 0; i < CALLERS; i++)
                if (callers[i].waits && (best < 0 || callers[i].us > callers[best].us))
                    best = i;
            if (best < 0)
                break;
            if (callers[best].caller == PORT_CALLER)
                len += sceClibSnprintf(line + len, sizeof(line) - len, " port");
            else
                len += sceClibSnprintf(line + len, sizeof(line) - len, " 0x%X", (unsigned)callers[best].caller);
            len += sceClibSnprintf(line + len, sizeof(line) - len, " (%u, %u ms)", (unsigned)callers[best].waits,
                                   (unsigned)(callers[best].us / 1000));
            callers[best].waits = 0;
        }
        waits = wait_us = 0;
        for (int i = 0; i < CALLERS; i++)
            callers[i].caller = callers[i].waits = callers[i].us = 0;
        sceKernelUnlockMutex(stats_lock, 1);
        if (n)
            l_info("[GPUWAIT] %u waits for the GPU lock, %u ms in all (last %d s); callers (waits, ms):%s", (unsigned)n,
                   (unsigned)(us / 1000), REPORT_MS / 1000, len ? line : " -");
    }
    return 0;
}
#endif

void ssx_assetunload_patch(void) {
    uintptr_t base = so_mod.text_base;
    AcquireThread = (void *)(base + OFF_AcquireThread);
    ReleaseThread = (void *)(base + OFF_ReleaseThread);
    FutexLock = (void *)(base + OFF_FutexLock);
    FutexUnlock = (void *)(base + OFF_FutexUnlock);
    gamerender_got = base + OFF_GameRender_ptr;
    // Starts "push {r4-r10, lr}; mov r5, r0"
    ReleaseAssetClass_orig = (void *)hook_arm_trampoline(&so_mod, base + OFF_ReleaseAssetClass,
                                                         (uintptr_t)ReleaseAssetClass_hook);
    // Starts "push {r3-r11, lr}; ldr r10, [r0, #20]"
    RBTreeIncrement = (void *)(base + OFF_RBTreeIncrement);
    RangeCache_Unload_orig = (void *)hook_arm_trampoline(&so_mod, base + OFF_RangeCache_Unload,
                                                         (uintptr_t)RangeCache_Unload_hook);
    if (!ReleaseAssetClass_orig || !RangeCache_Unload_orig)
        l_error("[ASSETUNLOAD] hooks failed (%p %p)", ReleaseAssetClass_orig, RangeCache_Unload_orig);
#ifdef DEBUG_SOLOADER
    if (!SSX_DIAG)
        return;
    GetThreadId = (void *)(base + OFF_GetThreadId);
    stats_lock = sceKernelCreateMutex("ssx_gpuwait", 0, 0, NULL);
    AcquireThread_orig = (void *)hook_arm_trampoline(&so_mod, base + OFF_AcquireThread, (uintptr_t)AcquireThread_hook);
    if (!AcquireThread_orig)
        l_error("[GPUWAIT] could not hook GameRender::AcquireThread");
    SceUID thid = sceKernelCreateThread("ssx_gpuwait", report_thread, 0x10000100, 0x2000, 0, 0, NULL);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, NULL);
#endif
}

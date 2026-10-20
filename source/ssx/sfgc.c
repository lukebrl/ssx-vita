/*
 * SSX port: Scaleform ActionScript garbage collection probe
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  sfgc.c
 * @brief Scaleform's AS2 garbage collection: heap limit fix, timings.
 *
 * A core dump taken during the 2 s freeze on the results screen has
 * FGGameSim in RefCountCollector::Collect, called from
 * HeapLimit::OnExceedLimit: an allocation took the UI movie's heap past its
 * limit. Other UI freezes look the same in the logs (logic frames with no
 * I/O or waits). This logs every collection over SFGC_LOG_US with what
 * started it and the heap limit state, to tell one long collection from
 * many short ones and to see which limit is being hit.
 *
 * It was many short ones: the game gives each UI movie's heap a user-level
 * limit of 2 MB, but the movies use ~3.3 MB. Past that limit
 * OnExceedLimit collects on every allocation that crosses the heap's
 * current limit, instead of once the heap has grown by its multiplier
 * (25%) since the last collection. Each collection takes 1-30 ms, and the
 * results screen ran 1100 of them in a row (1.7 s), the main menu 2000
 * (3.2 s). The user-level limit is raised to SFGC_USER_LIMIT, so
 * collections only follow growth; the movies' heaps live in
 * FE_SFGFX_GEN_A, which has over 15 MB free.
 *
 * MovieRoot::ProcessLoadMovieClip also runs a full collection (31 ms on the
 * results screen) whenever a loaded movie replaces another, and flushes the
 * text format caches with it. That call is removed: the replaced movie's
 * objects are collected with the next collection that heap growth starts.
 *
 * ASRefCountCollector::AdvanceFrame also collected every N frames (~2 s)
 * whenever there was any root: a collection walks everything reachable from
 * its roots, 23-36 ms in the menus even with 25 roots. The frame count
 * trigger is turned off; collections come from the root count threshold
 * (and heap growth) only.
 */

#include "ssx/sfgc.h"

#include <stdint.h>

#include <kubridge.h>
#include <psp2/kernel/processmgr.h>

#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

// libgame.so v0.0.8430
#define OFF_RefCountCollector_Collect 0x90a8b8 // RefCountCollector<323>::Collect(Stats*) (.part.70)
#define OFF_HeapLimit_OnExceedLimit   0x90ae14 // MemoryContextImpl::HeapLimit::OnExceedLimit(MemoryHeap*, unsigned)
// In MovieRoot::ProcessLoadMovieClip: bl HeapLimit::Collect(heap), made when
// a loaded movie replaces another
#define OFF_LoadMovieClip_Collect     0x90b78c
#define OFF_ASRefCountCollector_AdvanceFrame 0x90c1a0 // ASRefCountCollector::AdvanceFrame(unsigned*, unsigned*)
// ASRefCountCollector fields (AdvanceFrame): roots so far +8, frames since the
// last collection +52, root threshold +56, preset root count +84, frames
// between collections +88 (0: no collection by frame count)
#define COLLECTOR_ROOTS(c) (((uint32_t *)(c))[8 / 4])
#define COLLECTOR_THRESHOLD(c) (((uint32_t *)(c))[56 / 4])
#define COLLECTOR_PRESET(c) (((uint32_t *)(c))[84 / 4])
#define COLLECTOR_FRAMES_BETWEEN(c) (((uint32_t *)(c))[88 / 4])
#define ARM_NOP 0xe1a00000 // mov r0, r0

#define SFGC_LOG_US 10000
#define SFGC_USER_LIMIT (12 * 1024 * 1024)

static uint32_t (*Collect_orig)(void *collector, void *stats);
static uint32_t (*OnExceedLimit_orig)(void *limit, void *heap, uint32_t over);

static volatile uint32_t gc_count, gc_logged;
static volatile uint64_t gc_us, gc_max_us;
static volatile uint32_t exceed_count;

// Set by OnExceedLimit for the collection it may start (same thread)
static struct {
    uint32_t footprint, heap_limit, user_limit, last_footprint, current_limit, over;
    float multiplier;
    int valid;
} exceed;

#ifdef DEBUG_SOLOADER
static const char *caller_name(uint32_t off) {
    if (off >= 0x90acf8 && off < 0x90ade4) return "heap limit";
    if (off >= 0x90abb0 && off < 0x90ac7c) return "ForceCollect";
    if (off >= 0x90c058 && off < 0x90c1a0) return "emergency";
    if (off >= 0x90c1a0 && off < 0x90c4bc) return "AdvanceFrame";
    return "other";
}

static uint32_t Collect_hook(void *collector, void *stats) {
    uint32_t caller = (uint32_t)__builtin_return_address(0) - so_mod.text_base;
    uint32_t roots = *(uint32_t *)((uint8_t *)collector + 8);
    uint64_t t0 = sceKernelGetProcessTimeWide();
    uint32_t r = Collect_orig(collector, stats);
    uint64_t us = sceKernelGetProcessTimeWide() - t0;
    gc_count++;
    gc_us += us;
    if (us > gc_max_us)
        gc_max_us = us;
    if (us > SFGC_LOG_US) {
        gc_logged++;
        if (exceed.valid && !__builtin_strcmp(caller_name(caller), "heap limit"))
            l_info("[SFGC] %u ms collection, %u roots, from %s (game+0x%x): heap %u KB used, limit %u KB, "
                   "%u KB over; last collection at %u KB, multiplier %.2f, user limit %u KB",
                   (unsigned)(us / 1000), roots, caller_name(caller), caller, exceed.footprint / 1024,
                   exceed.heap_limit / 1024, exceed.over / 1024, exceed.last_footprint / 1024,
                   exceed.multiplier, exceed.user_limit / 1024);
        else
            l_info("[SFGC] %u ms collection, %u roots, from %s (game+0x%x)", (unsigned)(us / 1000), roots,
                   caller_name(caller), caller);
    }
    exceed.valid = 0;
    return r;
}

#endif

static uint32_t OnExceedLimit_hook(void *limit, void *heap, uint32_t over) {
    uint8_t *l = limit;
    uint32_t *user_limit = (uint32_t *)(l + 12);
    if (*user_limit && *user_limit < SFGC_USER_LIMIT)
        *user_limit = SFGC_USER_LIMIT;
#ifdef DEBUG_SOLOADER
    uint32_t (*footprint)(void *) = (*(uint32_t (***)(void *))heap)[21];
    exceed.footprint = footprint(heap);
    exceed.heap_limit = *(uint32_t *)((uint8_t *)heap + 48);
    exceed.user_limit = *(uint32_t *)(l + 12);
    exceed.last_footprint = *(uint32_t *)(l + 16);
    exceed.current_limit = *(uint32_t *)(l + 20);
    exceed.multiplier = *(float *)(l + 24);
    exceed.over = over;
    exceed.valid = 1;
    exceed_count++;
#endif
    uint32_t r = OnExceedLimit_orig(limit, heap, over);
    exceed.valid = 0;
    return r;
}

static void (*AdvanceFrame_orig)(void *collector, uint32_t *a, uint32_t *b);

// Without a preset root count the root threshold never triggers: then the
// frame count trigger stays, only less often
#define FALLBACK_FRAMES_BETWEEN 900 // 30 s at 30 fps

static void AdvanceFrame_hook(void *collector, uint32_t *a, uint32_t *b) {
    if (collector) {
        uint32_t frames = COLLECTOR_FRAMES_BETWEEN(collector);
        uint32_t want = COLLECTOR_PRESET(collector) ? 0 : FALLBACK_FRAMES_BETWEEN;
        if (frames != want && !(want && frames > want)) {
            l_info("[SFGC] collector %p: collections every %u frames -> %u (0: off); root threshold %u (preset %u), "
                   "%u roots now",
                   collector, frames, want, COLLECTOR_THRESHOLD(collector), COLLECTOR_PRESET(collector),
                   COLLECTOR_ROOTS(collector));
            COLLECTOR_FRAMES_BETWEEN(collector) = want;
        }
    }
    AdvanceFrame_orig(collector, a, b);
}

void ssx_sfgc_patch(void) {
    AdvanceFrame_orig = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_ASRefCountCollector_AdvanceFrame,
                                                    (uintptr_t)AdvanceFrame_hook);
    if (!AdvanceFrame_orig)
        l_error("[SFGC] could not hook ASRefCountCollector::AdvanceFrame");
    uint32_t *site = (uint32_t *)(so_mod.text_base + OFF_LoadMovieClip_Collect);
    if ((*site & 0xff000000) == 0xeb000000) { // a bl, as expected
        uint32_t nop = ARM_NOP;
        kuKernelCpuUnrestrictedMemcpy(site, &nop, sizeof(nop));
        kuKernelFlushCaches(site, sizeof(nop));
    } else {
        l_error("[SFGC] unexpected code in ProcessLoadMovieClip: 0x%08X", *site);
    }
    OnExceedLimit_orig = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_HeapLimit_OnExceedLimit,
                                                     (uintptr_t)OnExceedLimit_hook);
#ifdef DEBUG_SOLOADER
    Collect_orig = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_RefCountCollector_Collect,
                                               (uintptr_t)Collect_hook);
#endif
    if (!OnExceedLimit_orig)
        l_error("[SFGC] could not hook HeapLimit::OnExceedLimit");
}

void ssx_sfgc_report(void) {
    static uint32_t seen;
    if (gc_count == seen)
        return;
    l_info("[SFGC] %u collections (%u over %u ms), %.1f ms in total, longest %.1f ms; %u heap limit hits",
           gc_count - seen, gc_logged, SFGC_LOG_US / 1000, gc_us / 1000.0f, gc_max_us / 1000.0f, exceed_count);
    seen = gc_count;
    gc_logged = 0;
    gc_us = gc_max_us = 0;
    exceed_count = 0;
}

/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  memreport.c
 * @brief Logs how much of each EA MemoryFramework heap the game really uses.
 *
 * memory.cfg makes the game malloc every heap at full size upfront. Android
 * only backs the pages that get touched, the Vita backs all of them, so the
 * heaps have to be sized for the Vita from real peak usage, logged here.
 */

#include "ssx/memreport.h"
#include "ssx/texmem.h"
#include "ssx/spinwait.h"
#include "ssx/bufmem.h"
#include "ssx/renderprof.h"
#include "ssx/perf.h"

#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

// Offsets in libgame.so v0.0.8430 (local symbols)
#define OFF_GetNextAllocator  0x15d798 // MemoryFramework::GetNextAllocator(sEnumAllocator&, MemoryAllocatorUsage&)
#define OFF_GetFirstAllocator 0x15d904 // MemoryFramework::GetFirstAllocator(sEnumAllocator&, MemoryAllocatorUsage&)

// Layout read from GetNextAllocator's code
typedef struct {
    const char *name;
    void *allocator;      // IAllocator*
    uint32_t allocated;   // IAllocator::GetAllocated()
    uint32_t high_water;  // IAllocator::GetAllocatedHighWaterMark()
    uint32_t size;        // end - begin from IAllocator::GetRange()
    uint32_t pad[3];
} MemoryAllocatorUsage;

typedef struct {
    int index;
    uint32_t pad[3];
} sEnumAllocator;

static int (*GetFirstAllocator)(sEnumAllocator *, MemoryAllocatorUsage *);
static int (*GetNextAllocator)(sEnumAllocator *, MemoryAllocatorUsage *);

#define MB(x) ((x) / (1024.0f * 1024.0f))

void ssx_log_free_memory(const char *when) {
    SceKernelFreeMemorySizeInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    sceKernelGetFreeMemorySize(&info);
    l_info("[MEM] %s: free main %.1f MB, cdram %.1f MB, phycont %.1f MB",
           when, MB(info.size_user), MB(info.size_cdram), MB(info.size_phycont));

    // newlib heap (_newlib_heap_size_user): the EA heaps plus every plain malloc
    extern int _newlib_heap_size_user;
    struct mallinfo mi = mallinfo();
    l_info("[MEM] %s: newlib heap %.1f MB used, %.1f MB free", when, MB(mi.uordblks),
           MB((uint32_t)_newlib_heap_size_user - mi.arena + mi.fordblks));
}

// Since boot, sampled every second: the least free system memory and the
// newlib heap's extent and use at their highest. vitaGL's pools keep their
// own peaks (vglMemPeak, [GLMEM] lines).
static struct {
    int sampled;
    uint32_t min_main, min_cdram, min_phycont;
    uint32_t max_arena, max_used;
} peaks;

static void mem_sample(void) {
    SceKernelFreeMemorySizeInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    sceKernelGetFreeMemorySize(&info);
    struct mallinfo mi = mallinfo();
    if (!peaks.sampled || (uint32_t)info.size_user < peaks.min_main)
        peaks.min_main = info.size_user;
    if (!peaks.sampled || (uint32_t)info.size_cdram < peaks.min_cdram)
        peaks.min_cdram = info.size_cdram;
    if (!peaks.sampled || (uint32_t)info.size_phycont < peaks.min_phycont)
        peaks.min_phycont = info.size_phycont;
    if ((uint32_t)mi.arena > peaks.max_arena)
        peaks.max_arena = mi.arena;
    if ((uint32_t)mi.uordblks > peaks.max_used)
        peaks.max_used = mi.uordblks;
    peaks.sampled = 1;
}

static void log_mem_peaks(void) {
    extern int _newlib_heap_size_user;
    l_info("[MEMPEAK] since boot (sampled every second): newlib heap reached %.1f of %.1f MB (most in use %.1f MB); "
           "least free in the system: main %.1f MB, cdram %.1f MB, phycont %.1f MB",
           MB(peaks.max_arena), MB((uint32_t)_newlib_heap_size_user), MB(peaks.max_used), MB(peaks.min_main),
           MB(peaks.min_cdram), MB(peaks.min_phycont));
}

static void init_enum(void) {
    if (!GetFirstAllocator) {
        GetFirstAllocator = (void *)(so_mod.text_base + OFF_GetFirstAllocator);
        GetNextAllocator = (void *)(so_mod.text_base + OFF_GetNextAllocator);
    }
}

void *ssx_find_allocator(const char *name) {
    init_enum();
    sEnumAllocator e;
    MemoryAllocatorUsage u;
    memset(&e, 0, sizeof(e));
    memset(&u, 0, sizeof(u));
    if (GetFirstAllocator(&e, &u)) {
        do {
            if (u.name && u.allocator && !strcmp(u.name, name))
                return u.allocator;
            memset(&u, 0, sizeof(u));
        } while (GetNextAllocator(&e, &u));
    }
    return NULL;
}

unsigned ssx_allocator_largest_free(void *allocator) {
    // IAllocator vtable slot 16: GetLargestFreeBlock()
    uint32_t (**vtbl)(void *) = *(uint32_t (***)(void *))allocator;
    return vtbl[16](allocator);
}

void ssx_log_heaps(void) {
    if (!GetFirstAllocator) {
        GetFirstAllocator = (void *)(so_mod.text_base + OFF_GetFirstAllocator);
        GetNextAllocator = (void *)(so_mod.text_base + OFF_GetNextAllocator);
    }

    sEnumAllocator e;
    MemoryAllocatorUsage u;
    memset(&e, 0, sizeof(e));
    memset(&u, 0, sizeof(u));

    uint32_t total_size = 0, total_peak = 0;
    // The heap table (34 lines) with SSX_DIAG only; the game doesn't keep the
    // used and peak counters, so only the sizes and largest free blocks tell
    if (!SSX_DIAG) {
        ssx_log_free_memory("now");
        return;
    }
    l_info("[MEM] heap                      size MB    peak MB    used MB  largest free MB");
    if (GetFirstAllocator(&e, &u)) {
        do {
            uint32_t largest_free = 0;
            if (u.allocator) {
                // IAllocator vtable slot 16: GetLargestFreeBlock()
                uint32_t (**vtbl)(void *) = *(uint32_t (***)(void *))u.allocator;
                largest_free = vtbl[16](u.allocator);
            }
            l_info("[MEM] %-24s %8.2f   %8.2f   %8.2f   %8.2f",
                   u.name ? u.name : "?", MB(u.size), MB(u.high_water), MB(u.allocated), MB(largest_free));
            total_size += u.size;
            total_peak += u.high_water;
            memset(&u, 0, sizeof(u));
        } while (GetNextAllocator(&e, &u));
    }
    l_info("[MEM] TOTAL                    %8.2f   %8.2f", MB(total_size), MB(total_peak));
    ssx_log_free_memory("now");
}

#if defined(DEBUG_SOLOADER) && SSX_POOL_WALK
// Debug: what the UI pool really holds, block by block, from the allocator
// itself (its usage counters are off): bytes in use, bytes free, and how
// the free bytes are split. Holds the pool's lock while it walks.
#define OFF_PPMalloc_Lock 0x136cdc   // PPMalloc_Base<Mutex>::Lock()
#define OFF_PPMalloc_Unlock 0x136ccc // PPMalloc_Base<Mutex>::Unlock()
#define OFF_ReportHeap 0x151118      // GeneralAllocator::ReportHeap(callback, context, block types, copy, buffer, size)
#define POOL_NAME "FE_SFGFX_GEN_A"

// GeneralAllocator::BlockInfo
typedef struct {
    const void *core, *block;
    uint32_t block_size;
    const void *data;
    uint32_t data_size;
    char type;
    char mapped;
} BlockInfo;

typedef struct {
    uint32_t blocks[8], bytes[8], largest[8]; // by type bit
    uint32_t over_1m[8];                      // blocks of 1 MB and more
} pool_walk_t;

static int pool_block(const BlockInfo *b, void *ctx) {
    pool_walk_t *w = ctx;
    int t = 0;
    while (t < 7 && !(b->type & (1 << t)))
        t++;
    w->blocks[t]++;
    w->bytes[t] += b->block_size;
    if (b->block_size > w->largest[t])
        w->largest[t] = b->block_size;
    if (b->block_size >= 1024 * 1024)
        w->over_1m[t]++;
    return 1;
}

static void pool_report(void) {
    static void *pool;
    if (!pool)
        pool = ssx_find_allocator(POOL_NAME);
    if (!pool)
        return;
    void (*lock)(void *) = (void *)(so_mod.text_base + OFF_PPMalloc_Lock);
    void (*unlock)(void *) = (void *)(so_mod.text_base + OFF_PPMalloc_Unlock);
    int (*report)(void *, int (*)(const BlockInfo *, void *), void *, int, int, void *, uint32_t) =
        (void *)(so_mod.text_base + OFF_ReportHeap);
    void *ga = *(void **)((uint8_t *)pool + 24); // the GeneralAllocator
    static pool_walk_t w;
    memset(&w, 0, sizeof(w));
    uint32_t t0 = sceKernelGetProcessTimeLow();
    lock(pool);
    report(ga, pool_block, &w, 0xff, 0, NULL, 0);
    unlock(pool);
    uint32_t ms = (sceKernelGetProcessTimeLow() - t0) / 1000;
    char line[512];
    int n = 0;
    line[0] = 0;
    for (int t = 0; t < 8 && n < (int)sizeof(line); t++)
        if (w.blocks[t])
            n += snprintf(line + n, sizeof(line) - n, " type 0x%x: %u blocks, %u KB, largest %u KB, %u of 1 MB+;",
                          1 << t, w.blocks[t], w.bytes[t] / 1024, w.largest[t] / 1024, w.over_1m[t]);
    l_info("[POOL] %s:%s walked in %u ms", POOL_NAME, line, ms);
}
#endif

static int memreport_thread(SceSize args, void *argp) {
    int rounds = 0;
    while (1) {
        for (int s = 0; s < 10; s++) {
            sceKernelDelayThread(1000 * 1000);
            mem_sample();
        }
#if defined(DEBUG_SOLOADER) && SSX_POOL_WALK
        if (SSX_POOL_WALK && rounds++ % 3 == 0)
            pool_report();
#endif
        ssx_log_heaps();
        log_mem_peaks();
        ssx_texmem_report();
        ssx_spinwait_report();
        ssx_bufmem_report();
        perf_report();
        ssx_renderprof_report("now");
    }
    return 0;
}

void ssx_memreport_start(void) {
    SceUID thid = sceKernelCreateThread("ssx_memreport", memreport_thread, 0x10000100, 0x4000, 0, 0, NULL);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, NULL);
}

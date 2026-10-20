/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  allocprof.c
 * @brief Who allocates through MemoryFramework, and what it costs (debug aid).
 *
 * The UI profile has MemoryFramework::Category::Alloc as the biggest cost
 * in menus (~600 calls per frame), but they don't come from Scaleform's
 * heap. This times Category::Alloc and Category::Free and groups the
 * allocations by the first caller outside the allocators on the stack.
 */

#include "ssx/allocprof.h"

#include <stdint.h>
#include <string.h>

#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

// Off unless built with -DSSX_ALLOCPROF=1: it costs a stack scan per allocation
#if defined(DEBUG_SOLOADER) && SSX_ALLOCPROF

#define OFF_Category_Alloc 0x15a26c // MemoryFramework::Category::Alloc(unsigned, char const*, unsigned, unsigned, unsigned)
#define OFF_Category_Free 0x156388  // MemoryFramework::Category::Free(void*) [.part.9], what every free goes through
// libgame's .text (return addresses; .rodata pointers on the stack, such as
// allocation names, are not callers)
#define TEXT_START 0xed6b8
#define TEXT_END 0x147e0e0
// MemoryFramework and EA::Allocator code: skipped when looking for the caller
#define ALLOC_CODE_START 0x135000
#define ALLOC_CODE_END 0x15c000
#define NCALLERS 512 // power of two

typedef void *(*AllocFn)(void *, uint32_t, const char *, uint32_t, uint32_t, uint32_t);
static AllocFn Alloc_orig;
static void (*Free_orig)(void *, void *);

static struct {
    uint32_t caller, count, us;
} callers[NCALLERS];
static SceKernelLwMutexWork lock;
static volatile uint32_t allocs, frees, alloc_us, free_us;
// Sizes and alignments of the allocations UX::GFxHeap::Alloc makes (its call is at 0x6e1700)
#define GFXHEAP_CALL 0x6e1704
static const uint32_t bucket_max[] = { 32, 64, 128, 256, 512, 1024, 4096, 16384, 0xFFFFFFFF };
#define NBUCKETS (sizeof(bucket_max) / sizeof(bucket_max[0]))
static volatile uint32_t gfx_sizes[NBUCKETS], gfx_align_gt16, gfx_total;

static uint32_t first_caller(const uint32_t *sp) {
    for (int i = 0; i < 96; i++) {
        uint32_t v = sp[i];
        if (v > so_mod.text_base + TEXT_START && v < so_mod.text_base + TEXT_END) {
            uint32_t off = v - so_mod.text_base;
            if (off < ALLOC_CODE_START || off >= ALLOC_CODE_END)
                return off;
        }
    }
    return 0;
}

static void *Alloc_hook(void *cat, uint32_t size, const char *name, uint32_t flags, uint32_t align, uint32_t offset) {
    uint32_t t0 = sceKernelGetProcessTimeLow();
    void *r = Alloc_orig(cat, size, name, flags, align, offset);
    uint32_t us = sceKernelGetProcessTimeLow() - t0;
    uint32_t caller = first_caller((const uint32_t *)__builtin_frame_address(0));
    if (caller == GFXHEAP_CALL) {
        unsigned b = 0;
        while (size > bucket_max[b])
            b++;
        gfx_sizes[b]++;
        gfx_total++;
        if (align > 16)
            gfx_align_gt16++;
    }
    sceKernelLockLwMutex(&lock, 1, NULL);
    allocs++;
    alloc_us += us;
    uint32_t h = (caller * 2654435761u >> 20) & (NCALLERS - 1);
    for (int i = 0; i < 8; i++, h = (h + 1) & (NCALLERS - 1)) {
        if (callers[h].caller == caller || !callers[h].count) {
            callers[h].caller = caller;
            callers[h].count++;
            callers[h].us += us;
            break;
        }
    }
    sceKernelUnlockLwMutex(&lock, 1);
    return r;
}

static void Free_hook(void *cat, void *p) {
    uint32_t t0 = sceKernelGetProcessTimeLow();
    Free_orig(cat, p);
    uint32_t us = sceKernelGetProcessTimeLow() - t0;
    __atomic_add_fetch(&frees, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&free_us, us, __ATOMIC_RELAXED);
}

void ssx_allocprof_patch(void) {
    sceKernelCreateLwMutex(&lock, "allocprof", 0, 0, NULL);
    Free_orig = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_Category_Free, (uintptr_t)Free_hook);
    Alloc_orig = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + OFF_Category_Alloc, (uintptr_t)Alloc_hook);
    if (!Alloc_orig || !Free_orig)
        l_error("[ALLOCPROF] hooks failed: %p %p", Alloc_orig, Free_orig);
}

void ssx_allocprof_report(uint32_t frames) {
    if (!frames || !allocs)
        return;
    struct { uint32_t caller, count, us; } top[10];
    memset(top, 0, sizeof(top));
    sceKernelLockLwMutex(&lock, 1, NULL);
    for (int i = 0; i < NCALLERS; i++) {
        if (!callers[i].count)
            continue;
        int j = 10;
        while (j > 0 && top[j - 1].us < callers[i].us)
            j--;
        if (j < 10) {
            for (int k = 9; k > j; k--)
                top[k] = top[k - 1];
            top[j].caller = callers[i].caller;
            top[j].count = callers[i].count;
            top[j].us = callers[i].us;
        }
    }
    memset(callers, 0, sizeof(callers));
    uint32_t a = allocs, au = alloc_us, f = frees, fu = free_us;
    allocs = alloc_us = frees = free_us = 0;
    sceKernelUnlockLwMutex(&lock, 1);

    char line[768];
    int len = sceClibSnprintf(line, sizeof(line),
                              "[PERF] MemoryFramework per frame: %u allocs %.2f ms, %u frees %.2f ms; top callers (game+addr count ms):",
                              a / frames, au / 1000.0f / frames, f / frames, fu / 1000.0f / frames);
    for (int i = 0; i < 10 && top[i].count && len < (int)sizeof(line) - 40; i++)
        len += sceClibSnprintf(line + len, sizeof(line) - len, " 0x%X %u %.2f;", top[i].caller, top[i].count / frames,
                               top[i].us / 1000.0f / frames);
    l_info("%s", line);
    if (gfx_total) {
        len = sceClibSnprintf(line, sizeof(line), "[PERF] GFxHeap allocations per frame by size (<=32 64 128 256 512 1K 4K 16K more):");
        for (unsigned b = 0; b < NBUCKETS; b++)
            len += sceClibSnprintf(line + len, sizeof(line) - len, " %u", gfx_sizes[b] / frames);
        sceClibSnprintf(line + len, sizeof(line) - len, "; aligned over 16: %u", gfx_align_gt16 / frames);
        l_info("%s", line);
        memset((void *)gfx_sizes, 0, sizeof(gfx_sizes));
        gfx_total = gfx_align_gt16 = 0;
    }
}

#else
void ssx_allocprof_patch(void) {}
void ssx_allocprof_report(uint32_t frames) { (void)frames; }
#endif

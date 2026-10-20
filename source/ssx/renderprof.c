/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  renderprof.c
 * @brief Logs which code holds the most memory in the RENDER_GEN heap.
 *
 * RENDER_GEN serves several memory categories (Render, Render::RNA,
 * Render::GameRender...), each with its own allocator object. Their
 * Alloc/AllocAligned/Free vtable slots are redirected here; every block is
 * recorded with the libgame address that asked for it, and the call sites
 * holding the most live memory are logged (every 10 s and when the game runs
 * out of memory). Other allocators sharing those vtables pass straight
 * through.
 */

#include "ssx/renderprof.h"

#include <stdint.h>
#include <string.h>

#include <kubridge.h>
#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

// MemoryFramework::GetICoreAllocator(char const*) in libgame.so v0.0.8430
#define OFF_GetICoreAllocator 0x15b838

// The categories memory.cfg puts in RENDER_GEN
static const char *const categories[] = {
    "Render", "Render::StrInternal", "Render::Particles", "Render::GameRender",
    "Render::RNA", "Render::RwMovie",
};
#define NCATEGORIES (sizeof(categories) / sizeof(categories[0]))

typedef void *(*AllocFn)(void *self, uint32_t size, const char *name, uint32_t flags);
typedef void *(*AllocAlignedFn)(void *self, uint32_t size, const char *name, uint32_t flags, uint32_t align, uint32_t offset);
typedef void (*FreeFn)(void *self, void *block, uint32_t size);

static void *targets[NCATEGORIES];
static int ntargets;

// Original slots of each vtable we patched
static struct {
    void **vtbl;
    AllocFn alloc;
    AllocAlignedFn alloc_aligned;
    FreeFn free;
} vtables[NCATEGORIES];
static int nvtables;

#define MAX_SITES 512
#define MAX_BLOCKS 65536 // power of two
#define MAX_PROBES 64     // slots looked at per lookup (deleted slots never become empty again)

typedef struct {
    uint32_t caller; // offset in libgame.so
    uint32_t live_bytes, live_count;
} site;

typedef struct {
    void *ptr; // NULL: empty, (void *)1: deleted
    uint32_t size;
    uint16_t site;
} block;

static site sites[MAX_SITES];
static int nsites;
static block blocks[MAX_BLOCKS];
static SceKernelLwMutexWork lock;
static uint32_t untracked;

static int is_target(void *self) {
    for (int i = 0; i < ntargets; i++)
        if (targets[i] == self)
            return 1;
    return 0;
}

static int vtable_of(void *self) {
    void **vtbl = *(void ***)self;
    for (int i = 0; i < nvtables; i++)
        if (vtables[i].vtbl == vtbl)
            return i;
    return 0; // cannot happen: only patched vtables call us
}

static int site_index(uint32_t caller) {
    for (int i = 0; i < nsites; i++)
        if (sites[i].caller == caller)
            return i;
    if (nsites == MAX_SITES)
        return MAX_SITES - 1; // everything else lands in the last one
    sites[nsites].caller = caller;
    return nsites++;
}

static uint32_t slot_of(void *p) {
    return ((uintptr_t)p >> 4) * 2654435761u & (MAX_BLOCKS - 1);
}

static void record(void *p, uint32_t size, uintptr_t ra) {
    if (!p)
        return;
    uint32_t caller = ra - so_mod.text_base;
    sceKernelLockLwMutex(&lock, 1, NULL);
    int s = site_index(caller);
    uint32_t i = slot_of(p);
    for (int n = 0; n < MAX_PROBES; n++, i = (i + 1) & (MAX_BLOCKS - 1)) {
        if (blocks[i].ptr == NULL || blocks[i].ptr == (void *)1) {
            blocks[i].ptr = p;
            blocks[i].size = size;
            blocks[i].site = s;
            sites[s].live_bytes += size;
            sites[s].live_count++;
            sceKernelUnlockLwMutex(&lock, 1);
            return;
        }
    }
    untracked++;
    sceKernelUnlockLwMutex(&lock, 1);
}

static void forget(void *p) {
    if (!p)
        return;
    sceKernelLockLwMutex(&lock, 1, NULL);
    uint32_t i = slot_of(p);
    for (int n = 0; n < MAX_PROBES && blocks[i].ptr; n++, i = (i + 1) & (MAX_BLOCKS - 1)) {
        if (blocks[i].ptr == p) {
            site *s = &sites[blocks[i].site];
            s->live_bytes -= blocks[i].size;
            s->live_count--;
            blocks[i].ptr = (void *)1;
            break;
        }
    }
    sceKernelUnlockLwMutex(&lock, 1);
}

static void *Alloc_hook(void *self, uint32_t size, const char *name, uint32_t flags) {
    void *p = vtables[vtable_of(self)].alloc(self, size, name, flags);
    if (is_target(self))
        record(p, size, (uintptr_t)__builtin_return_address(0));
    return p;
}

static void *AllocAligned_hook(void *self, uint32_t size, const char *name, uint32_t flags, uint32_t align, uint32_t offset) {
    void *p = vtables[vtable_of(self)].alloc_aligned(self, size, name, flags, align, offset);
    if (is_target(self))
        record(p, size, (uintptr_t)__builtin_return_address(0));
    return p;
}

static void Free_hook(void *self, void *p, uint32_t size) {
    if (is_target(self))
        forget(p);
    vtables[vtable_of(self)].free(self, p, size);
}

int ssx_renderprof_start(void) {
    if (ntargets)
        return 1;
    uintptr_t base = so_mod.text_base;
    void *(*GetICoreAllocator)(const char *) = (void *)(base + OFF_GetICoreAllocator);
    void *found[NCATEGORIES];
    int nfound = 0;
    for (unsigned i = 0; i < NCATEGORIES; i++) {
        void *a = GetICoreAllocator(categories[i]);
        if (!a)
            return 0;
        found[nfound++] = a;
    }

    sceKernelCreateLwMutex(&lock, "renderprof", 0, 0, NULL);
    // Record the originals of every vtable before patching any, so that a
    // hook never runs without them
    for (int i = 0; i < nfound; i++) {
        void **vtbl = *(void ***)found[i];
        int known = 0;
        for (int v = 0; v < nvtables; v++)
            known |= vtables[v].vtbl == vtbl;
        if (!known) {
            vtables[nvtables].vtbl = vtbl;
            vtables[nvtables].alloc = vtbl[2];
            vtables[nvtables].alloc_aligned = vtbl[3];
            vtables[nvtables].free = vtbl[4];
            nvtables++;
        }
        targets[ntargets++] = found[i];
    }
    // Blocks allocated before this point are unknown: their frees are ignored
    void *hooks[3] = { (void *)Alloc_hook, (void *)AllocAligned_hook, (void *)Free_hook };
    for (int v = 0; v < nvtables; v++)
        kuKernelCpuUnrestrictedMemcpy(&vtables[v].vtbl[2], hooks, sizeof(hooks));
    l_info("[RENDERPROF] tracking %d Render category allocators (%d vtables)", ntargets, nvtables);
    return 1;
}

void ssx_renderprof_report(const char *when) {
    if (!ntargets)
        return;
    site top[16];
    int ntop = 0;
    uint32_t total = 0;
    static uint8_t picked[MAX_SITES];
    sceKernelLockLwMutex(&lock, 1, NULL);
    memset(picked, 0, sizeof(picked));
    for (int i = 0; i < nsites; i++)
        total += sites[i].live_bytes;
    for (ntop = 0; ntop < 16; ntop++) {
        int best = -1;
        for (int i = 0; i < nsites; i++)
            if (!picked[i] && (best < 0 || sites[i].live_bytes > sites[best].live_bytes))
                best = i;
        if (best < 0)
            break;
        picked[best] = 1;
        top[ntop] = sites[best];
    }
    sceKernelUnlockLwMutex(&lock, 1);

    l_info("[RENDERPROF] %s: %.2f MB live in RENDER_GEN since tracking began, %d call sites%s",
           when, total / 1048576.0f, nsites, untracked ? " (table full, some untracked)" : "");
    for (int i = 0; i < ntop && top[i].live_bytes; i++)
        l_info("[RENDERPROF]   game+0x%07X  %8.1f KB in %u blocks", top[i].caller,
               top[i].live_bytes / 1024.0f, top[i].live_count);
}

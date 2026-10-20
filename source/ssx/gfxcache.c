/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  gfxcache.c
 * @brief Scaleform's heap (UX::GFxHeap): small blocks reused, big ones out
 *        of the UI pool when it is fragmented.
 *
 * Small blocks. When a UI screen loads, Scaleform creates and frees
 * thousands of small objects (ActionScript values, strings, display
 * objects): ~21500 allocations and ~18000 frees in the first logic frame of
 * the race results screen. Each goes through UX::GFxHeap (two allocation-rule
 * tree lookups, limit checks, an atomic usage update) into EA's
 * MemoryFramework (a lock, the category's allocators asked in turn, an
 * aligned GeneralAllocator allocation). Freed blocks of up to MAX_SMALL bytes
 * are kept as they are, in per-heap lists by 16-byte size class, and handed
 * out again for requests of their heap that they hold, up to KEEP_PER_HEAP
 * per heap and KEEP_TOTAL in all; past that a freed block goes back to the
 * heap at once. A kept block is still the heap's own block: GFxHeap::Alloc
 * puts a 32-byte header before every block (user address ^ 0x1337C0DE, raw
 * address, heap, size asked for, user address, allocation id), and Realloc,
 * GetUsableSize and TrackerData::GetOwner read it. So reuse costs no memory
 * per block. 70-75% of the allocations in menus are served this way.
 *
 * Big blocks. Blocks of BIG_MIN and up go to system memory instead when the
 * UI pool (FE_SFGFX_GEN_A) has no free run that holds them with BIG_MARGIN
 * to spare. The pool doesn't leak but fragments: in use 19.3 MB in a first
 * race and 19.6 MB in the second, but its largest free run fell from 15.2 to
 * 11.5 MB (walked with GeneralAllocator::ReportHeap). The race loading
 * screens needed 1.8 MB in one piece twice per race load (a 16-bit copy of
 * the image, and a temporary one when it is uploaded); that allocation
 * failing ended sessions after a few races.
 *
 * History. A version that cut blocks from 16-64 KB chunks and kept every
 * freed block ran the UI pool out of memory after 55 minutes: Scaleform
 * keeps one heap per UI file for the whole session (CreateHeap reuses heaps
 * by name, none is destroyed), so a screen's freed blocks were only ever
 * reusable by that same screen. The next one took each block with a header
 * of its own on top of GFxHeap's and padding to 32 bytes: 32-56 bytes more
 * for blocks that are mostly 16-64 bytes, and the pool fragmented twice as
 * fast (largest free run 7 MB in the first race instead of 14 MB).
 */

#include "ssx/gfxcache.h"

#include <stdint.h>
#include <string.h>

#include <kubridge.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

#include "ssx/memreport.h"
#include "utils/logger.h"

extern so_module so_mod;

// libgame.so v0.0.8430, UX::GFxHeap
#define OFF_Alloc 0x6e1608         // Alloc(unsigned size, unsigned align, Scaleform::AllocInfo const*)
#define OFF_Free 0x6e1888          // Free(void*)
#define OFF_Realloc 0x6e18b4       // Realloc(void*, unsigned)
#define OFF_GetUsableSize 0x6e14cc // GetUsableSize(void const*): "subs r0, r1, #0; bxeq lr; b 0x6e146c"
#define OFF_UsableSizeOf 0x6e146c  // its tail, usable size of the block at r0
#define OFF_Dtor 0x6e1234          // ~GFxHeap(): heaps don't seem to die, but if one does

#define MAX_SMALL 1024 // covers the 520-byte AS2 stack page every sprite allocates (AvmSprite -> Environment)
#define CLASSES (MAX_SMALL / 16 + 1)
#define MAX_HEAPS 512 // power of two
#define KEEP_PER_HEAP (64 * 1024)
#define KEEP_TOTAL (2 * 1024 * 1024)
#define HDR 32 // GFxHeap's header
#define ALIGN 32
#define GFX_MAGIC 0x1337C0DEu

#define BIG_MIN (512 * 1024)
#define BIG_MARGIN (2 * 1024 * 1024)   // the pool keeps a run this much bigger than the block, else system memory
#define BIG_SYS_KEEP (2 * 1024 * 1024) // system memory left free
#define MAX_BIG 16
#define BIG_POOL "FE_SFGFX_GEN_A"
#define MAGIC_BIG 0xB16B10C5u

// UX::GFxHeap's header, the HDR bytes before each block (GFxHeap::Alloc
// writes the first 22). Big blocks of ours mark themselves in the tail.
typedef struct {
    uint32_t magic; // user address ^ GFX_MAGIC
    uint32_t raw;   // where the allocation starts
    uint32_t heap;  // the owning heap
    uint32_t size;  // the size asked for
    uint32_t user;  // the user address
    uint16_t info;  // allocation id
    uint16_t unused;
    uint32_t pad;
    uint32_t check; // big blocks: MAGIC_BIG ^ user address
} header_t;
_Static_assert(sizeof(header_t) == HDR, "the header must fill HDR");

static void *(*Alloc_orig)(void *heap, uint32_t size, uint32_t align, const void *info);
static void (*Free_orig)(void *heap, void *p);
static void *(*Realloc_orig)(void *heap, void *p, uint32_t size);
static uint32_t (*UsableSizeOf)(const void *p);
static void *(*Dtor_orig)(void *heap);

static inline header_t *hdr_of(const void *p) {
    return (header_t *)((uint8_t *)p - HDR);
}

// --- Bytes in use ---------------------------------------------------------------
//
// Usable sizes of the blocks in Scaleform's heaps (kept blocks included), for
// the [PERF] report.

static volatile uint32_t live_bytes, live_peak;

static inline void live_add(int32_t n) {
    uint32_t v = __atomic_add_fetch(&live_bytes, (uint32_t)n, __ATOMIC_RELAXED);
    if (n > 0 && v > live_peak)
        live_peak = v; // racy, close enough for a report
}

static void *alloc_counted(void *heap, uint32_t size, uint32_t align, const void *info) {
    void *r = Alloc_orig(heap, size, align, info);
    if (r)
        live_add((int32_t)UsableSizeOf(r));
    return r;
}

static void free_counted(void *heap, void *p) {
    if (p)
        live_add(-(int32_t)UsableSizeOf(p));
    Free_orig(heap, p);
}

// --- Small blocks ---------------------------------------------------------------

typedef struct {
    void *heap;
    void *head[CLASSES]; // free blocks (user pointers), next pointer in their first word
    uint32_t bytes;      // in the free lists
} heap_lists_t;

// A destroyed heap's slot: keeps the probe chains of the others intact
#define HEAP_GONE ((void *)1)

static heap_lists_t heaps[MAX_HEAPS];
static uint32_t kept_total;
static SceKernelLwMutexWork lock;
static int enabled;
static volatile uint32_t hits, misses, kept, released, passed_size, passed_align, no_slot, heaps_released;

// GFxHeap's header of p, if it has a valid one
static inline header_t *gfx_header(const void *p) {
    if (!p || ((uintptr_t)p & 15))
        return NULL;
    header_t *h = hdr_of(p);
    uint32_t u = (uint32_t)(uintptr_t)p;
    return (h->magic == (u ^ GFX_MAGIC) && h->user == u) ? h : NULL;
}

// Caller holds the lock
static heap_lists_t *lists_of(void *heap, int create) {
    uint32_t h = (((uint32_t)(uintptr_t)heap >> 4) * 2654435761u) >> 23; // 9 bits: MAX_HEAPS
    heap_lists_t *reuse = NULL;
    for (int i = 0; i < MAX_HEAPS; i++, h = (h + 1) & (MAX_HEAPS - 1)) {
        heap_lists_t *l = &heaps[h];
        if (l->heap == heap)
            return l;
        if (l->heap == HEAP_GONE) {
            if (!reuse)
                reuse = l;
            continue;
        }
        if (!l->heap) {
            if (!reuse)
                reuse = l;
            break;
        }
    }
    if (!create || !reuse)
        return NULL;
    memset(reuse, 0, sizeof(*reuse));
    reuse->heap = heap;
    return reuse;
}

// A small block from the heap's lists, or a new one; NULL if size or align
// aren't for the lists
static void *alloc_small(void *heap, uint32_t size, uint32_t align, const void *info) {
    if (!enabled || size == 0 || size > MAX_SMALL || align > ALIGN) {
        if (enabled && size > MAX_SMALL)
            passed_size++;
        else if (enabled && align > ALIGN)
            passed_align++;
        return NULL;
    }
    uint32_t cls = (size + 15) >> 4;
    uint32_t mask = align ? align - 1 : 0;

    sceKernelLockLwMutex(&lock, 1, NULL);
    heap_lists_t *l = lists_of(heap, 1);
    void *p = l ? l->head[cls] : NULL;
    if (p && !((uintptr_t)p & mask)) {
        uint32_t cost = hdr_of(p)->size + HDR;
        l->head[cls] = *(void **)p;
        l->bytes -= cost;
        kept_total -= cost;
        hits++;
        sceKernelUnlockLwMutex(&lock, 1);
        return p;
    }
    if (!l)
        no_slot++;
    sceKernelUnlockLwMutex(&lock, 1);
    misses++;
    // Asked for the whole class: the header then says cls * 16, and once
    // freed the block goes back to this class (a 20-byte block would be
    // filed under 16 bytes, out of reach of the next 20-byte request)
    return alloc_counted(heap, cls * 16, align, info);
}

// Keeps a freed block for reuse if it is small and the caps allow; 0 if not
static int keep(void *p) {
    header_t *h = gfx_header(p);
    if (!h)
        return 0;
    // The class it can serve: requests up to cls * 16 bytes
    uint32_t cls = h->size >> 4;
    if (!cls || cls >= CLASSES)
        return 0;
    uint32_t cost = h->size + HDR;
    sceKernelLockLwMutex(&lock, 1, NULL);
    heap_lists_t *l = lists_of((void *)(uintptr_t)h->heap, 1);
    if (l && l->bytes + cost <= KEEP_PER_HEAP && kept_total + cost <= KEEP_TOTAL) {
        *(void **)p = l->head[cls];
        l->head[cls] = p;
        l->bytes += cost;
        kept_total += cost;
        kept++;
        sceKernelUnlockLwMutex(&lock, 1);
        return 1;
    }
    sceKernelUnlockLwMutex(&lock, 1);
    released++;
    return 0;
}

// --- Big blocks -----------------------------------------------------------------

typedef struct {
    void *p;
    SceUID uid;
    uint32_t size, bytes;
} big_t;

static big_t bigs[MAX_BIG];
static volatile int nbig;
static uint32_t big_bytes;
static SceKernelLwMutexWork big_lock;
static void *big_pool; // the pool's IAllocator

// A block in system memory, when the pool is short of a run for it; NULL to
// take it from the heap as usual
static void *alloc_big(void *heap, uint32_t size, uint32_t align) {
    if (!big_pool)
        big_pool = ssx_find_allocator(BIG_POOL);
    if (!big_pool)
        return NULL;
    uint32_t largest = ssx_allocator_largest_free(big_pool);
    if (largest >= size + BIG_MARGIN)
        return NULL;
    uint32_t off = align > HDR ? align : HDR;
    if (off > 4096)
        return NULL;
    uint32_t bytes = (off + size + 0xfff) & ~0xfffu;
    SceKernelFreeMemorySizeInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    if (sceKernelGetFreeMemorySize(&info) < 0 || (uint32_t)info.size_user < bytes + BIG_SYS_KEEP)
        return NULL;
    SceUID uid = sceKernelAllocMemBlock("gfxbig", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, bytes, NULL);
    if (uid < 0)
        return NULL;
    void *base = NULL;
    sceKernelGetMemBlockBase(uid, &base);
    uint8_t *p = (uint8_t *)base + off;
    sceKernelLockLwMutex(&big_lock, 1, NULL);
    int slot = -1;
    for (int i = 0; i < MAX_BIG; i++)
        if (!bigs[i].p) {
            slot = i;
            break;
        }
    if (slot < 0) {
        sceKernelUnlockLwMutex(&big_lock, 1);
        sceKernelFreeMemBlock(uid);
        return NULL;
    }
    // GFxHeap's header, so that TrackerData::GetOwner finds the heap
    header_t *h = hdr_of(p);
    uint32_t u = (uint32_t)(uintptr_t)p;
    memset(h, 0, sizeof(*h));
    h->magic = u ^ GFX_MAGIC;
    h->raw = u - HDR;
    h->heap = (uint32_t)(uintptr_t)heap;
    h->size = size;
    h->user = u;
    h->check = MAGIC_BIG ^ u;
    bigs[slot] = (big_t){ p, uid, size, bytes };
    nbig++;
    big_bytes += bytes;
    uint32_t now = big_bytes;
    sceKernelUnlockLwMutex(&big_lock, 1);
    l_info("[GFXBIG] %u KB block in system memory (%s's largest free run is %u KB); %u KB there now", size / 1024,
           BIG_POOL, largest / 1024, now / 1024);
    live_add((int32_t)size);
    return p;
}

// The size of a big block of ours, 0 if p isn't one
static uint32_t big_size(const void *p) {
    if (!nbig || !p)
        return 0;
    uint32_t size = 0;
    sceKernelLockLwMutex(&big_lock, 1, NULL);
    for (int i = 0; i < MAX_BIG; i++)
        if (bigs[i].p == p) {
            size = bigs[i].size;
            break;
        }
    sceKernelUnlockLwMutex(&big_lock, 1);
    return size;
}

static void free_big(void *p) {
    SceUID uid = -1;
    sceKernelLockLwMutex(&big_lock, 1, NULL);
    for (int i = 0; i < MAX_BIG; i++)
        if (bigs[i].p == p) {
            uid = bigs[i].uid;
            live_add(-(int32_t)bigs[i].size);
            hdr_of(p)->check = 0;
            hdr_of(p)->magic = 0;
            big_bytes -= bigs[i].bytes;
            bigs[i].p = NULL;
            nbig--;
            break;
        }
    sceKernelUnlockLwMutex(&big_lock, 1);
    if (uid >= 0)
        sceKernelFreeMemBlock(uid);
}

// --- GFxHeap's functions --------------------------------------------------------

static void *gfx_alloc(void *heap, uint32_t size, uint32_t align, const void *info) {
    void *p = NULL;
    if (size >= BIG_MIN)
        p = alloc_big(heap, size, align);
    if (!p)
        p = alloc_small(heap, size, align, info);
    if (!p)
        p = alloc_counted(heap, size, align, info);
    return p;
}

static void gfx_free(void *heap, void *p) {
    if (big_size(p))
        free_big(p);
    else if (!(enabled && keep(p)))
        free_counted(heap, p);
}

static void *Realloc_hook(void *heap, void *p, uint32_t size) {
    uint32_t big = big_size(p);
    if (big) {
        void *n = NULL;
        if (size) {
            n = gfx_alloc((void *)(uintptr_t)hdr_of(p)->heap, size, ALIGN, NULL);
            if (!n)
                return NULL;
            memcpy(n, p, big < size ? big : size);
        }
        free_big(p);
        return n;
    }
    // GFxHeap::Realloc keeps the block if it is big enough, else takes a new
    // one through the heap's Alloc (our hook: counted there) and frees the
    // old one with FreeInternal (not hooked): only that is left to count
    int32_t old = p ? (int32_t)UsableSizeOf(p) : 0;
    void *r = Realloc_orig(heap, p, size);
    if (r && r != p)
        live_add(-old);
    return r;
}

static uint32_t GetUsableSize_hook(void *heap, const void *p) {
    if (!p)
        return 0;
    uint32_t big = big_size(p);
    return big ? big : UsableSizeOf(p);
}

// A heap being destroyed: its kept blocks go back to it first
static void *Dtor_hook(void *heap) {
    void *blocks[CLASSES];
    sceKernelLockLwMutex(&lock, 1, NULL);
    heap_lists_t *l = lists_of(heap, 0);
    if (l) {
        memcpy(blocks, l->head, sizeof(blocks));
        kept_total -= l->bytes;
        l->heap = HEAP_GONE;
        heaps_released++;
    }
    sceKernelUnlockLwMutex(&lock, 1);
    if (l)
        for (int c = 0; c < CLASSES; c++)
            for (void *p = blocks[c]; p;) {
                void *next = *(void **)p;
                free_counted(heap, p);
                p = next;
            }
    return Dtor_orig(heap);
}

// --- Debug: what the allocations cost the logic thread per frame --------------
//
// Every call counted, one in SAMPLE_EVERY timed and scaled up; calls over
// SLOW_US are counted on their own.

#ifdef DEBUG_SOLOADER
#define SAMPLE_EVERY 8
#define SLOW_US 1000

static uintptr_t sim_key;
static struct {
    uint32_t allocs, frees, sampled_allocs, sampled_frees;
    uint32_t alloc_us, free_us; // of the sampled calls
    uint32_t slow_allocs, slow_alloc_us, slow_frees, slow_free_us;
} fr;

static inline uintptr_t thread_key(void) {
    uintptr_t v;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
    return v;
}

void gfxcache_set_thread(void) {
    sim_key = thread_key();
}

void gfxcache_frame(uint32_t prev_frame_ms, const char *ui, int report) {
    if (report && fr.allocs + fr.frees)
        l_info("[GFXHEAP] %u ms logic frame on %s: %u allocations ~%.1f ms, %u frees ~%.1f ms (logic thread, "
               "1 in %u timed); slow ones among the timed: %u allocations %u ms, %u frees %u ms",
               prev_frame_ms, ui, fr.allocs,
               fr.sampled_allocs ? fr.alloc_us / 1000.0f * fr.allocs / fr.sampled_allocs : 0.0f, fr.frees,
               fr.sampled_frees ? fr.free_us / 1000.0f * fr.frees / fr.sampled_frees : 0.0f, SAMPLE_EVERY,
               fr.slow_allocs, fr.slow_alloc_us / 1000, fr.slow_frees, fr.slow_free_us / 1000);
    memset(&fr, 0, sizeof(fr));
}

static void *Alloc_hook(void *heap, uint32_t size, uint32_t align, const void *info) {
    if (!sim_key || thread_key() != sim_key || ++fr.allocs % SAMPLE_EVERY)
        return gfx_alloc(heap, size, align, info);
    uint32_t t0 = sceKernelGetProcessTimeLow();
    void *r = gfx_alloc(heap, size, align, info);
    uint32_t us = sceKernelGetProcessTimeLow() - t0;
    if (us > SLOW_US) {
        fr.slow_allocs++;
        fr.slow_alloc_us += us;
    } else {
        fr.alloc_us += us;
        fr.sampled_allocs++;
    }
    return r;
}

static void Free_hook(void *heap, void *p) {
    if (!sim_key || thread_key() != sim_key || ++fr.frees % SAMPLE_EVERY) {
        gfx_free(heap, p);
        return;
    }
    uint32_t t0 = sceKernelGetProcessTimeLow();
    gfx_free(heap, p);
    uint32_t us = sceKernelGetProcessTimeLow() - t0;
    if (us > SLOW_US) {
        fr.slow_frees++;
        fr.slow_free_us += us;
    } else {
        fr.free_us += us;
        fr.sampled_frees++;
    }
}
#else
void gfxcache_set_thread(void) {}

void gfxcache_frame(uint32_t prev_frame_ms, const char *ui, int report) {
    (void)prev_frame_ms;
    (void)ui;
    (void)report;
}

#define Alloc_hook gfx_alloc
#define Free_hook gfx_free
#endif

// --- Setup and report -----------------------------------------------------------

void ssx_gfxcache_patch(void) {
    uintptr_t base = so_mod.text_base;
    sceKernelCreateLwMutex(&lock, "gfxcache", 0, 0, NULL);
    sceKernelCreateLwMutex(&big_lock, "gfxbig", 0, 0, NULL);
    UsableSizeOf = (void *)(base + OFF_UsableSizeOf);

    // GetUsableSize is three instructions with a branch: replaced whole. The
    // others keep their originals behind trampolines. All or nothing.
    const uint32_t *gus = (const uint32_t *)(base + OFF_GetUsableSize);
    if (gus[0] != 0xE2510000 || gus[1] != 0x012FFF1E) {
        l_error("[GFXCACHE] unexpected GFxHeap::GetUsableSize (0x%08X 0x%08X): off", gus[0], gus[1]);
        return;
    }
    Free_orig = (void *)hook_arm_trampoline(&so_mod, base + OFF_Free, (uintptr_t)Free_hook);
    Realloc_orig = (void *)hook_arm_trampoline(&so_mod, base + OFF_Realloc, (uintptr_t)Realloc_hook);
    Dtor_orig = (void *)hook_arm_trampoline(&so_mod, base + OFF_Dtor, (uintptr_t)Dtor_hook);
    if (!Free_orig || !Realloc_orig || !Dtor_orig) {
        l_error("[GFXCACHE] hooks failed (%p %p %p): off", Free_orig, Realloc_orig, Dtor_orig);
        return; // the hooks that went in pass through: no block of ours exists
    }
    hook_arm(base + OFF_GetUsableSize, (uintptr_t)GetUsableSize_hook);
    kuKernelFlushCaches((void *)(base + OFF_GetUsableSize), 8);
    // Alloc last: our blocks only exist once every function knows them
    Alloc_orig = (void *)hook_arm_trampoline(&so_mod, base + OFF_Alloc, (uintptr_t)Alloc_hook);
    if (!Alloc_orig) {
        l_error("[GFXCACHE] could not hook GFxHeap::Alloc: off");
        return;
    }
    enabled = 1;
    l_info("[GFXCACHE] small Scaleform blocks kept for reuse (up to %u KB per heap, %u KB in all); "
           "blocks of %u KB and up in system memory when %s is short",
           KEEP_PER_HEAP / 1024, KEEP_TOTAL / 1024, BIG_MIN / 1024, BIG_POOL);
}

void ssx_gfxcache_report(void) {
    if (live_peak) {
        l_info("[PERF] Scaleform heaps: %u KB in use, peak %u KB since the last report", live_bytes / 1024,
               live_peak / 1024);
        live_peak = live_bytes;
    }
    if (hits || misses || kept || released) {
        int nheaps = 0;
        sceKernelLockLwMutex(&lock, 1, NULL);
        for (int i = 0; i < MAX_HEAPS; i++)
            if (heaps[i].heap && heaps[i].heap != HEAP_GONE)
                nheaps++;
        uint32_t total = kept_total;
        sceKernelUnlockLwMutex(&lock, 1);
        l_info("[PERF] Scaleform small blocks: %u reused, %u new; %u kept, %u given back; %u KB kept in %d heaps; "
               "%u heaps destroyed; passed on: %u too big, %u aligned over %d, %u no slot",
               hits, misses, kept, released, total / 1024, nheaps, heaps_released, passed_size, passed_align, ALIGN,
               no_slot);
    }
    hits = misses = kept = released = passed_size = passed_align = no_slot = heaps_released = 0;
}

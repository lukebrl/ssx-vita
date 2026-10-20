/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  scopecache.c
 * @brief Caches RNA::ScopeC::FindParm.
 *
 * Shaders look their parameters up by name every time they are bound:
 * FindParm searches the scope's blocks by name (strcmp down a tree), then the
 * block's parameters (strcmp down a list), ~1200 times per race frame for
 * ~4 ms. The results are cached by scope, type, index and a hash of the two
 * names, and a hit is checked against the parameter it returns (type, name,
 * index) before use.
 *
 * Any change to any scope (blocks or parameters added, removed or deleted,
 * scopes created or destroyed) bumps a generation number that empties the
 * cache; those functions are hooked for it. If one of them can't be hooked,
 * the cache stays off. Only the render thread uses the cache.
 *
 * In front of it sits a cache keyed by the name pointers themselves, for
 * names that are string literals in libgame (almost all of them: the
 * shaders' parameter tables). Those can't change, so a hit there skips both
 * hashing the names and comparing them: ~3800 lookups per race frame,
 * 2000 of them from RNAX::OGLESProgram::Bind.
 *
 * RNA::ScopeC::FindBlockByName is cached the same way (by scope and name,
 * same generation): ResolveParm and AddParm call it on every material bind,
 * ~290 times a frame in a race with many terrain chunks, ~6 us each (it
 * interns the name, then walks the scope's block tree with strcmp). Only
 * names that are libgame string literals, compared by pointer: the block
 * doesn't hold its name (the tree node does), so a hit by hash can't be
 * checked.
 */

#include "ssx/scopecache.h"

#include <stdint.h>
#include <string.h>

#include <so_util/so_util.h>

#include "ssx/glprof.h"
#include "utils/logger.h"

extern so_module so_mod;

// libgame.so v0.0.8430
#define OFF_ScopeC_FindParm 0xdb60cc // RNA::ScopeC::FindParm(char const*, TypeID, char const*, unsigned) const
#define OFF_ScopeC_FindBlockByName 0xdb5ff8 // RNA::ScopeC::FindBlockByName(char const*) const
// ScopeParmC fields FindParm compares
#define PARM_TYPE(p)  (*(uint32_t *)((uint8_t *)(p) + 28))
#define PARM_NAME(p)  (*(const char **)((uint8_t *)(p) + 40))
#define PARM_INDEX(p) (*(uint32_t *)((uint8_t *)(p) + 52))

static volatile uint32_t generation = 1;
static int tracking; // all the functions that change scopes are hooked

uint32_t ssx_scopecache_generation(void) {
    return tracking ? __atomic_load_n(&generation, __ATOMIC_SEQ_CST) : 0;
}

// --- Functions that change scopes: bump the generation ---------------------

typedef uint64_t (*Fn8)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);

// Up to 8 words of arguments, passed on as they are (extra ones unused)
#define MUTATOR(n)                                                                          \
    static Fn8 mut_orig_##n;                                                                \
    static uint64_t mut_hook_##n(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,        \
                                 uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) {      \
        __atomic_add_fetch(&generation, 1, __ATOMIC_SEQ_CST);                               \
        uint64_t r = mut_orig_##n(a0, a1, a2, a3, a4, a5, a6, a7);                          \
        __atomic_add_fetch(&generation, 1, __ATOMIC_SEQ_CST);                               \
        return r;                                                                           \
    }
MUTATOR(0) MUTATOR(1) MUTATOR(2) MUTATOR(3) MUTATOR(4) MUTATOR(5) MUTATOR(6) MUTATOR(7) MUTATOR(8)

// What FindParm searches: the scope's blocks and their parameters (ParmListC,
// the per-shader lists, doesn't change it). ResolveParm adds through AddBlock
// and ScopeBlockC::AddParm; ~ScopeC deletes its blocks' parameters through
// ScopeBlockC::DeleteAllParms.
static const struct {
    uint32_t addr;
    const char *name;
} mutators[] = {
    { 0xdbdba8, "ScopeC::ScopeC" },
    { 0xdc0638, "ScopeC::~ScopeC" },
    { 0xdb5d64, "ScopeC::AddBlock" },
    { 0xdb6b38, "ScopeC::AddParm" },
    { 0xdb7764, "ScopeC::DeleteUnusedParms" },
    { 0xdb56fc, "ScopeBlockC::ScopeBlockC" },
    { 0xdb5c4c, "ScopeBlockC::AddParm" },
    { 0xdb57fc, "ScopeBlockC::DeleteAllParms" },
    { 0xdb5a78, "ScopeBlockC::DeleteUnusedParms" },
};
#define NMUTATORS (sizeof(mutators) / sizeof(mutators[0]))

static Fn8 *const mut_origs[] = {
    &mut_orig_0, &mut_orig_1, &mut_orig_2, &mut_orig_3, &mut_orig_4, &mut_orig_5, &mut_orig_6, &mut_orig_7, &mut_orig_8,
};
static void *const mut_hooks[] = {
    mut_hook_0, mut_hook_1, mut_hook_2, mut_hook_3, mut_hook_4, mut_hook_5, mut_hook_6, mut_hook_7, mut_hook_8,
};
_Static_assert(sizeof(mut_hooks) / sizeof(mut_hooks[0]) == NMUTATORS, "one hook per mutator");

// --- The cache ----------------------------------------------------------------

#define CACHE_SIZE 2048 // power of two

static struct {
    uint32_t gen;
    const void *scope;
    uint32_t hblock, hparm, type, index;
    void *result;
} cache[CACHE_SIZE];

#define L1_SIZE 1024 // power of two

static struct {
    uint32_t gen;
    const void *scope;
    const char *block, *parm;
    uint32_t type, index;
    void *result;
} l1[L1_SIZE];

static uintptr_t image_start, image_end;

static inline int in_image(const char *s) {
    return (uintptr_t)s >= image_start && (uintptr_t)s < image_end;
}

static void *(*FindParm_orig)(const void *scope, const char *block, uint32_t type, const char *parm, uint32_t index);
static volatile uint32_t hits, misses, null_misses;

static inline uintptr_t thread_key(void) {
    uintptr_t v;
    __asm__ volatile("mrc p15, 0, %0, c13, c0, 3" : "=r"(v));
    return v;
}

static inline uint32_t fnv(const char *s, uint32_t h) {
    if (!s)
        return h ^ 0x5bd1e995;
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 0x01000193;
    return h;
}

static void *FindParm_hook(const void *scope, const char *block, uint32_t type, const char *parm, uint32_t index) {
    uintptr_t rt = glprof_render_thread();
    if (!rt || thread_key() != rt)
        return FindParm_orig(scope, block, type, parm, index);

    uint32_t gen = __atomic_load_n(&generation, __ATOMIC_SEQ_CST);
    int literal = parm && in_image(parm) && (!block || in_image(block));
    uint32_t s1 = 0;
    if (literal) {
        s1 = (((uintptr_t)scope >> 4) ^ ((uintptr_t)block >> 2) ^ ((uintptr_t)parm * 7) ^ type * 131 ^ index * 7919) &
             (L1_SIZE - 1);
        if (l1[s1].gen == gen && l1[s1].parm == parm && l1[s1].block == block && l1[s1].scope == scope &&
            l1[s1].type == type && l1[s1].index == index) {
            void *r = l1[s1].result;
            if (PARM_TYPE(r) == type && PARM_INDEX(r) == index) {
                hits++;
                return r;
            }
        }
    }
    uint32_t hb = fnv(block, 0x811C9DC5), hp = fnv(parm, 0x811C9DC5);
    uint32_t slot = (((uintptr_t)scope >> 4) ^ hb * 31 ^ hp ^ type * 131 ^ index * 7919) & (CACHE_SIZE - 1);

    if (cache[slot].gen == gen && cache[slot].scope == scope && cache[slot].hblock == hb &&
        cache[slot].hparm == hp && cache[slot].type == type && cache[slot].index == index) {
        void *r = cache[slot].result;
        if (r && PARM_TYPE(r) == type && PARM_INDEX(r) == index && parm && PARM_NAME(r) &&
            strcmp(PARM_NAME(r), parm) == 0) {
            hits++;
            if (literal) {
                l1[s1].gen = gen;
                l1[s1].scope = scope;
                l1[s1].block = block;
                l1[s1].parm = parm;
                l1[s1].type = type;
                l1[s1].index = index;
                l1[s1].result = r;
            }
            return r;
        }
    }

    misses++;
    void *r = FindParm_orig(scope, block, type, parm, index);
    null_misses += !r;
    // Not found: not cached (the parameter may be added without a hooked call)
    if (r && __atomic_load_n(&generation, __ATOMIC_SEQ_CST) == gen) {
        cache[slot].gen = gen;
        cache[slot].scope = scope;
        cache[slot].hblock = hb;
        cache[slot].hparm = hp;
        cache[slot].type = type;
        cache[slot].index = index;
        cache[slot].result = r;
    }
    return r;
}

// --- FindBlockByName ------------------------------------------------------------

#define BLOCK_CACHE_SIZE 512 // power of two

static struct {
    uint32_t gen;
    const void *scope;
    const char *name; // a literal, compared by pointer
    void *result;
} blocks[BLOCK_CACHE_SIZE];

static void *(*FindBlockByName_orig)(const void *scope, const char *name);
static volatile uint32_t block_hits, block_misses;

static void *FindBlockByName_hook(const void *scope, const char *name) {
    uintptr_t rt = glprof_render_thread();
    if (!name || !rt || thread_key() != rt || !in_image(name))
        return FindBlockByName_orig(scope, name);
    uint32_t gen = __atomic_load_n(&generation, __ATOMIC_SEQ_CST);
    uint32_t h = (uint32_t)(uintptr_t)name * 2654435761u;
    uint32_t slot = (((uintptr_t)scope >> 4) ^ h ^ (h >> 15)) & (BLOCK_CACHE_SIZE - 1);
    if (blocks[slot].gen == gen && blocks[slot].scope == scope && blocks[slot].name == name) {
        block_hits++;
        return blocks[slot].result;
    }
    block_misses++;
    void *r = FindBlockByName_orig(scope, name);
    // Not found: not cached (the block may be added without a hooked call)
    if (r && __atomic_load_n(&generation, __ATOMIC_SEQ_CST) == gen) {
        blocks[slot].gen = gen;
        blocks[slot].scope = scope;
        blocks[slot].name = name;
        blocks[slot].result = r;
    }
    return r;
}

void ssx_scopecache_patch(void) {
    uintptr_t base = so_mod.text_base;
    image_start = so_mod.text_base;
    image_end = so_mod.text_base + so_mod.text_size;
    for (unsigned i = 0; i < NMUTATORS; i++) {
        *mut_origs[i] = (Fn8)hook_arm_trampoline(&so_mod, base + mutators[i].addr, (uintptr_t)mut_hooks[i]);
        if (!*mut_origs[i]) {
            l_error("[SCOPECACHE] could not hook %s: cache off", mutators[i].name);
            return;
        }
    }
    tracking = 1;
    FindBlockByName_orig =
        (void *)hook_arm_trampoline(&so_mod, base + OFF_ScopeC_FindBlockByName, (uintptr_t)FindBlockByName_hook);
    if (!FindBlockByName_orig)
        l_error("[SCOPECACHE] could not hook ScopeC::FindBlockByName");
    FindParm_orig = (void *)hook_arm_trampoline(&so_mod, base + OFF_ScopeC_FindParm, (uintptr_t)FindParm_hook);
    if (!FindParm_orig)
        l_error("[SCOPECACHE] could not hook ScopeC::FindParm: cache off");
    else
        l_info("[SCOPECACHE] caching RNA::ScopeC::FindParm");
}

void ssx_scopecache_report(uint32_t frames) {
    extern volatile uint32_t ssx_shaderbind_attr_null;
    if (frames && FindParm_orig)
        l_info("[PERF] FindParm per frame: %u cached, %u searched (%u not found, %u of them OGLESProgram::Bind's "
               "attributes); FindBlockByName %u cached, %u searched",
               hits / frames, misses / frames, null_misses / frames, ssx_shaderbind_attr_null / frames,
               block_hits / frames, block_misses / frames);
    hits = misses = null_misses = block_hits = block_misses = 0;
    ssx_shaderbind_attr_null = 0;
}

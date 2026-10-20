/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  movielife.c
 * @brief Debug: which Scaleform movies are created and destroyed, and when.
 *
 * Scaleform's heaps grow by a few MB with each race and menu round
 * (15 MB at boot, 28 MB after two races), and what grows is mostly movie
 * data: AS2 bytecode (ActionBufferData::Read), placed objects, shapes. A
 * movie's data (MovieDataDef, one per .gfx) should die with the last screen
 * using it. This logs every MovieDataDef made and destroyed with its file,
 * and keeps counts of MovieDefImpl (a bound movie) and MovieImpl (a running
 * movie instance), to see what is never released.
 */

#include "ssx/movielife.h"

#include <stdint.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#ifdef DEBUG_SOLOADER

// libgame.so v0.0.8430, Scaleform::GFx
#define OFF_MovieDataDef_ctor 0x841014 // MovieDataDef(ResourceKey const&, MovieDataType, char const*, MemoryHeap*, bool, unsigned)
#define OFF_MovieDataDef_dtor 0x83626c // ~MovieDataDef() (D1)
#define OFF_MovieDefImpl_ctor 0x83aac0 // MovieDefImpl(MovieDataDef*, MovieDefBindStates*, LoaderImpl*, unsigned, StateBagImpl*, MemoryHeap*, bool, unsigned)
#define OFF_MovieDefImpl_dtor 0x83adac // ~MovieDefImpl() (D1)
#define OFF_MovieImpl_ctor 0x8461e0    // MovieImpl(MemoryHeap*)
#define OFF_MovieImpl_dtor 0x8548c0    // ~MovieImpl() (D1)

#define MAX_DEFS 1024
#define NAME_LEN 48

typedef struct {
    void *def;
    char name[NAME_LEN];
} def_t;

static def_t defs[MAX_DEFS];
static int32_t data_defs, def_impls, movies;
static SceKernelLwMutexWork lock;

static void *(*DataDef_ctor)(void *, const void *, int, const char *, void *, int, unsigned);
static void *(*DataDef_dtor)(void *);
static void *(*DefImpl_ctor)(void *, void *, void *, void *, unsigned, void *, void *, int, unsigned);
static void *(*DefImpl_dtor)(void *);
static void *(*Movie_ctor)(void *, void *);
static void *(*Movie_dtor)(void *);

// The file name without the folders
static const char *base_name(const char *s) {
    if (!s)
        return "?";
    const char *b = s;
    for (const char *p = s; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

static void *DataDef_ctor_hook(void *self, const void *key, int type, const char *file, void *heap, int b,
                               unsigned u) {
    void *r = DataDef_ctor(self, key, type, file, heap, b, u);
    const char *name = base_name(file);
    sceKernelLockLwMutex(&lock, 1, NULL);
    int32_t n = ++data_defs;
    for (int i = 0; i < MAX_DEFS; i++)
        if (!defs[i].def) {
            defs[i].def = self;
            strncpy(defs[i].name, name, NAME_LEN - 1);
            defs[i].name[NAME_LEN - 1] = 0;
            break;
        }
    sceKernelUnlockLwMutex(&lock, 1);
    l_info("[MOVIELIFE] + data %s (%p); alive: %d data, %d bound, %d running", name, self, n, def_impls, movies);
    return r;
}

static void *DataDef_dtor_hook(void *self) {
    char name[NAME_LEN] = "?";
    sceKernelLockLwMutex(&lock, 1, NULL);
    int32_t n = --data_defs;
    for (int i = 0; i < MAX_DEFS; i++)
        if (defs[i].def == self) {
            memcpy(name, defs[i].name, NAME_LEN);
            defs[i].def = NULL;
            break;
        }
    sceKernelUnlockLwMutex(&lock, 1);
    l_info("[MOVIELIFE] - data %s (%p); alive: %d data, %d bound, %d running", name, self, n, def_impls, movies);
    return DataDef_dtor(self);
}

static void *DefImpl_ctor_hook(void *self, void *data, void *bind, void *loader, unsigned flags, void *states,
                               void *heap, int b, unsigned u) {
    __atomic_add_fetch(&def_impls, 1, __ATOMIC_RELAXED);
    return DefImpl_ctor(self, data, bind, loader, flags, states, heap, b, u);
}

static void *DefImpl_dtor_hook(void *self) {
    __atomic_sub_fetch(&def_impls, 1, __ATOMIC_RELAXED);
    return DefImpl_dtor(self);
}

static void *Movie_ctor_hook(void *self, void *heap) {
    __atomic_add_fetch(&movies, 1, __ATOMIC_RELAXED);
    return Movie_ctor(self, heap);
}

static void *Movie_dtor_hook(void *self) {
    __atomic_sub_fetch(&movies, 1, __ATOMIC_RELAXED);
    return Movie_dtor(self);
}

#define HOOK(var, off, fn)                                                                                             \
    do {                                                                                                               \
        var = (void *)hook_arm_trampoline(&so_mod, so_mod.text_base + (off), (uintptr_t)(fn));                         \
        if (!var)                                                                                                      \
            l_error("[MOVIELIFE] could not hook 0x%X", (unsigned)(off));                                               \
    } while (0)

void ssx_movielife_patch(void) {
    if (!SSX_DIAG)
        return;
    sceKernelCreateLwMutex(&lock, "movielife", 0, 0, NULL);
    HOOK(DataDef_dtor, OFF_MovieDataDef_dtor, DataDef_dtor_hook);
    HOOK(DefImpl_dtor, OFF_MovieDefImpl_dtor, DefImpl_dtor_hook);
    HOOK(Movie_dtor, OFF_MovieImpl_dtor, Movie_dtor_hook);
    HOOK(DataDef_ctor, OFF_MovieDataDef_ctor, DataDef_ctor_hook);
    HOOK(DefImpl_ctor, OFF_MovieDefImpl_ctor, DefImpl_ctor_hook);
    HOOK(Movie_ctor, OFF_MovieImpl_ctor, Movie_ctor_hook);
}

void ssx_movielife_report(void) {
    if (!SSX_DIAG)
        return;
    static int32_t last_data = -1, last_bound = -1, last_running = -1;
    if (data_defs == last_data && def_impls == last_bound && movies == last_running)
        return;
    last_data = data_defs;
    last_bound = def_impls;
    last_running = movies;
    l_info("[MOVIELIFE] alive: %d movie data, %d bound movies, %d running movies", last_data, last_bound,
           last_running);
}

#else
void ssx_movielife_patch(void) {}
void ssx_movielife_report(void) {}
#endif

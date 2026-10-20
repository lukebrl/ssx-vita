/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  bufmem.c
 * @brief Frees the RAM copy the engine keeps of every static vertex and
 *        index buffer.
 *
 * RNA::StreamPlatC (vertex buffers) and RNA::IndicesC (index buffers) keep a
 * copy of their contents in the "Render" heaps for their whole life: Lock()
 * returns it, Unlock() uploads it with glBufferData/glBufferSubData, and
 * RecreateOnDeviceReset uploads it again after Android loses the GL context
 * (never on the Vita). With texture copies gone (texmem.c), these filled
 * RENDER_GEN on the first level after the tutorial.
 *
 * After Unlock() uploads a GL_STATIC_DRAW buffer created by the world or
 * terrain loaders (see allowed_callers), its copy is freed. Should
 * the game Lock() it again, it gets a new copy filled from the buffer's GPU
 * data (vitaGL keeps it in CPU-visible memory), so writing part of it and
 * uploading the whole on Unlock() stays correct. Dynamic buffers keep theirs.
 *
 * Destroy() returns early when the copy is gone, before it deletes the GL
 * buffer ("if (!data) return;" comes first), so the Destroy hook deletes it
 * itself for those buffers. Without that, every world and terrain buffer of
 * a level stayed in GPU memory after the level: ~10 MB per race, until
 * terrain stopped loading and the game crashed on the 4th race.
 */

#include "ssx/bufmem.h"

#include <stdint.h>
#include <string.h>

#include <psp2/kernel/clib.h>

#include <vitaGL.h>
#include <kubridge.h>
#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#define GL_STATIC_DRAW_ 0x88E4

// Per class: functions and fields, in libgame.so v0.0.8430
typedef struct {
    const char *name;
    uint32_t lock, unlock;        // Lock(unsigned), Unlock()
    uint32_t create, destroy;     // Create(...), Destroy()
    uint32_t globals_insn;        // "add rX, pc, rX" giving the globals block...
    uint32_t globals_literal;     // ...from this literal (see Destroy())
    uint32_t allocator_off;       // allocator pointer in the globals block
    uint32_t gl_name, size, usage, data, mapped; // fields
    uint32_t device;              // RNA::DevicePlatC*, whose slot 30 deletes GL buffers
} buf_class;

static const buf_class classes[] = {
    // RNA::StreamPlatC: Destroy() at 0xdc1ee8 frees +52 with globals+124
    { "vertex", 0xdacb38, 0xdba9cc, 0xdc1284, 0xdc1ee8, 0xdc1f34, 0xdc1fcc, 124, 40, 44, 48, 52, 56, 36 },
    // RNA::IndicesC: Destroy() at 0xdc272c frees +40 with globals+128
    { "index", 0xdb869c, 0xdb86a4, 0xdc14d0, 0xdc272c, 0xdc2778, 0xdc2810, 128, 28, 32, 36, 40, 44, 24 },
};
#define NCLASSES (sizeof(classes) / sizeof(classes[0]))

// Only buffers created from these call sites (return addresses of the
// Create() calls) lose their copy: world and terrain geometry, which is
// filled once. Character meshes (GeomData, skinning) keep theirs: freeing
// them left the player model as a mess of stretched triangles.
static const uint32_t allowed_callers[] = {
    0xd6cc94, // RNAX::RNASceneC::SerializeInStream
    0xfe0aa0, // StaticInstancesRenderObj::BakeRNAMeshToGPU
    0xfec758, 0xfec794, 0xfed0bc, // WorldResourceAllocator::CreateGameObject (terrain)
    0xd6c81c, // RNAX::RNASceneC::SerializeInIndices
    0xfe0ac0, // StaticInstancesRenderObj::BakeRNAMeshToGPU
    0xfec82c, 0xfec948, 0xfeca60, 0xfed1e4, // WorldResourceAllocator::CreateGameObject (terrain)
};

#define NSITES (sizeof(allowed_callers) / sizeof(allowed_callers[0]))

// Per call site: copies freed, and Lock()s of buffers whose copy was freed
static volatile uint32_t site_freed_bytes[NSITES], site_freed[NSITES], site_relocks[NSITES];

// Buffers whose copy may be freed (open addressing, backward-shift deletion),
// with the call site that created them; KEEP once the game locked one again
// after its copy was freed: a buffer it still writes to keeps its copy
#define SET_SIZE 16384 // power of two
#define KEEP 0x80
static void *freeable[SET_SIZE];
static uint8_t meta[SET_SIZE]; // site index | KEEP
static SceKernelLwMutexWork set_lock;

static uint32_t set_slot(void *p) {
    return ((uintptr_t)p >> 3) * 2654435761u & (SET_SIZE - 1);
}

static void set_add(void *p, int site) {
    sceKernelLockLwMutex(&set_lock, 1, NULL);
    uint32_t i = set_slot(p);
    for (int n = 0; n < SET_SIZE; n++, i = (i + 1) & (SET_SIZE - 1)) {
        if (freeable[i] == p || !freeable[i]) {
            freeable[i] = p;
            meta[i] = site;
            break;
        }
    }
    sceKernelUnlockLwMutex(&set_lock, 1);
}

// meta of p, or -1 if not in the set; with or_meta ORed into it
static int set_get(void *p, uint8_t or_meta) {
    int m = -1;
    sceKernelLockLwMutex(&set_lock, 1, NULL);
    for (uint32_t i = set_slot(p); freeable[i]; i = (i + 1) & (SET_SIZE - 1)) {
        if (freeable[i] == p) {
            meta[i] |= or_meta;
            m = meta[i];
            break;
        }
    }
    sceKernelUnlockLwMutex(&set_lock, 1);
    return m;
}

static void set_remove(void *p) {
    sceKernelLockLwMutex(&set_lock, 1, NULL);
    uint32_t i = set_slot(p);
    while (freeable[i] && freeable[i] != p)
        i = (i + 1) & (SET_SIZE - 1);
    if (freeable[i]) {
        // Shift back the entries after it that would no longer be found
        uint32_t j = i;
        while (1) {
            j = (j + 1) & (SET_SIZE - 1);
            if (!freeable[j])
                break;
            uint32_t home = set_slot(freeable[j]);
            // freeable[j] stays if its home lies cyclically in (i, j]
            if ((i < j) ? (home > i && home <= j) : (home > i || home <= j))
                continue;
            freeable[i] = freeable[j];
            meta[i] = meta[j];
            i = j;
        }
        freeable[i] = NULL;
    }
    sceKernelUnlockLwMutex(&set_lock, 1);
}

#define FIELD(obj, off) (*(uint32_t *)((uint8_t *)(obj) + (off)))

// EA::Allocator::ICoreAllocator: Alloc(size, name, flags) is slot 2,
// Free(block, size) slot 4. The block starts with the data size; the data
// pointer is block + 16.
typedef void *(*AllocFn)(void *self, uint32_t size, const char *name, uint32_t flags);
typedef void (*FreeFn)(void *self, void *block, uint32_t size);

static uint8_t *globals[NCLASSES];
static void (*unlock_orig[NCLASSES])(void *buf);

static volatile uint32_t freed_bytes, freed_count, restored_count;

static void *allocator(int c) {
    void *a = *(void **)(globals[c] + classes[c].allocator_off);
    return a ? a : *(void **)(globals[c] + 20);
}

static void after_unlock(int c, void *buf) {
    const buf_class *k = &classes[c];
    uint8_t *data = (uint8_t *)FIELD(buf, k->data);
    void *alloc = allocator(c);
    if (!data || !alloc || FIELD(buf, k->mapped) || !FIELD(buf, k->gl_name) ||
        FIELD(buf, k->usage) != GL_STATIC_DRAW_)
        return;
    int m = set_get(buf, 0);
    if (m < 0 || (m & KEEP))
        return;

    uint8_t *block = data - 16;
    uint32_t size = *(uint32_t *)block;
    FIELD(buf, k->data) = 0;
    ((FreeFn)(*(void ***)alloc)[4])(alloc, block, 0);

    __atomic_add_fetch(&site_freed_bytes[m], size, __ATOMIC_RELAXED);
    __atomic_add_fetch(&site_freed[m], 1, __ATOMIC_RELAXED);
    uint32_t total = __atomic_add_fetch(&freed_bytes, size, __ATOMIC_RELAXED);
    __atomic_add_fetch(&freed_count, 1, __ATOMIC_RELAXED);
    if ((total >> 23) != ((total - size) >> 23)) // a line every 8 MB
        l_info("[BUFMEM] freed %u buffer copies, %.1f MB", freed_count, total / (1024.0f * 1024.0f));
}

static void *before_lock(int c, void *buf) {
    const buf_class *k = &classes[c];
    if (FIELD(buf, k->data) || !FIELD(buf, k->gl_name))
        return NULL;

    // A buffer whose copy was freed: give it a new one with the GPU data, and
    // keep it from now on (the game still writes to this buffer)
    int m = set_get(buf, KEEP);
    if (m >= 0)
        __atomic_add_fetch(&site_relocks[m & ~KEEP], 1, __ATOMIC_RELAXED);
    uint32_t size = FIELD(buf, k->size);
    void *alloc = allocator(c);
    if (!size || !alloc)
        return NULL;
    uint8_t *block = ((AllocFn)(*(void ***)alloc)[2])(alloc, size + 16, NULL, 1);
    if (!block) {
        l_error("[BUFMEM] no memory to restore a %u-byte %s buffer copy", size, k->name);
        return NULL;
    }
    *(uint32_t *)block = size;
    GLsizei gpu_size = 0;
    const void *gpu = vglGetBufferData(FIELD(buf, k->gl_name), &gpu_size);
    uint32_t n = gpu ? ((uint32_t)gpu_size < size ? (uint32_t)gpu_size : size) : 0;
    if (n)
        memcpy(block + 16, gpu, n);
    if (n < size)
        memset(block + 16 + n, 0, size - n);
    FIELD(buf, k->data) = (uint32_t)(block + 16);
    __atomic_add_fetch(&restored_count, 1, __ATOMIC_RELAXED);
    return block + 16;
}

// Lock() is just "return data" (ldr r0, [r0, #data]; bx lr): replaced whole
static void *Lock_vertex(void *buf, uint32_t flags) { before_lock(0, buf); return (void *)FIELD(buf, classes[0].data); }
static void *Lock_index(void *buf, uint32_t flags) { before_lock(1, buf); return (void *)FIELD(buf, classes[1].data); }
static void Unlock_vertex(void *buf) { unlock_orig[0](buf); after_unlock(0, buf); }
static void Unlock_index(void *buf) { unlock_orig[1](buf); after_unlock(1, buf); }

typedef int (*CreateFn)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static CreateFn create_orig[NCLASSES];
static void (*destroy_orig[NCLASSES])(void *buf);

static void after_create(void *buf, uintptr_t ra) {
    uint32_t caller = ra - so_mod.text_base;
    for (unsigned i = 0; i < sizeof(allowed_callers) / sizeof(allowed_callers[0]); i++) {
        if (allowed_callers[i] == caller) {
            set_add(buf, i);
            return;
        }
    }
}

// Create() takes up to 8 words of arguments; extra ones are passed on unused
static int Create_vertex(void *b, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) {
    int ret = create_orig[0](b, a1, a2, a3, a4, a5, a6, a7);
    after_create(b, (uintptr_t)__builtin_return_address(0));
    return ret;
}
static int Create_index(void *b, uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) {
    int ret = create_orig[1](b, a1, a2, a3, a4, a5, a6, a7);
    after_create(b, (uintptr_t)__builtin_return_address(0));
    return ret;
}
static volatile uint32_t gl_deleted;

static void before_destroy(int c, void *buf) {
    const buf_class *k = &classes[c];
    set_remove(buf);
    void *device = (void *)FIELD(buf, k->device);
    if (FIELD(buf, k->data) || !FIELD(buf, k->gl_name) || !device)
        return;
    typedef void (*DeleteFn)(void *self, int n, uint32_t *names);
    ((DeleteFn)(*(void ***)device)[30])(device, 1, (uint32_t *)((uint8_t *)buf + k->gl_name));
    FIELD(buf, k->gl_name) = 0;
    __atomic_add_fetch(&gl_deleted, 1, __ATOMIC_RELAXED);
}

static void Destroy_vertex(void *buf) { before_destroy(0, buf); destroy_orig[0](buf); }
static void Destroy_index(void *buf) { before_destroy(1, buf); destroy_orig[1](buf); }

static void *const create_hooks[NCLASSES] = { Create_vertex, Create_index };
static void *const destroy_hooks[NCLASSES] = { Destroy_vertex, Destroy_index };
static void *const lock_hooks[NCLASSES] = { Lock_vertex, Lock_index };
static void *const unlock_hooks[NCLASSES] = { Unlock_vertex, Unlock_index };

void ssx_bufmem_patch(void) {
    uintptr_t base = so_mod.text_base;
    sceKernelCreateLwMutex(&set_lock, "bufmem_set", 0, 0, NULL);
    for (unsigned c = 0; c < NCLASSES; c++) {
        const buf_class *k = &classes[c];
        globals[c] = (uint8_t *)(base + k->globals_insn + 8 + *(uint32_t *)(base + k->globals_literal));
        // Destroy and Create first: nothing is freed for buffers not in the set
        destroy_orig[c] = (void *)hook_arm_trampoline(&so_mod, base + k->destroy, (uintptr_t)destroy_hooks[c]);
        create_orig[c] = destroy_orig[c] ? (void *)hook_arm_trampoline(&so_mod, base + k->create, (uintptr_t)create_hooks[c]) : NULL;
        if (!destroy_orig[c] || !create_orig[c]) {
            l_error("[BUFMEM] could not hook %s Create/Destroy, its copies stay in RAM", k->name);
            continue;
        }
        // Lock first: only Unlock starts freeing, so a buffer is never left
        // without a copy if hooking Unlock fails. Lock is two instructions,
        // "ldr r0, [r0, #data]; bx lr", which the hook replaces whole.
        const uint32_t *lock = (const uint32_t *)(base + k->lock);
        if (lock[0] != (0xE5900000 | k->data) || lock[1] != 0xE12FFF1E) {
            l_error("[BUFMEM] unexpected %s Lock (0x%08X 0x%08X), its copies stay in RAM", k->name, lock[0], lock[1]);
            continue;
        }
        hook_arm(base + k->lock, (uintptr_t)lock_hooks[c]);
        kuKernelFlushCaches((void *)(base + k->lock), 8);
        unlock_orig[c] = (void *)hook_arm_trampoline(&so_mod, base + k->unlock, (uintptr_t)unlock_hooks[c]);
        if (!unlock_orig[c])
            l_error("[BUFMEM] could not hook %s Unlock, its copies stay in RAM", k->name);
    }
}

void ssx_bufmem_report(void) {
    l_info("[BUFMEM] %u buffer copies freed (%.1f MB), %u restored by Lock, %u GL buffers deleted for Destroy()",
           freed_count, freed_bytes / (1024.0f * 1024.0f), restored_count, gl_deleted);
    char line[512];
    int len = sceClibSnprintf(line, sizeof(line), "[BUFMEM] per creator (game+addr: freed MB / count / relocked):");
    for (unsigned i = 0; i < NSITES && len < (int)sizeof(line) - 48; i++)
        if (site_freed[i] || site_relocks[i])
            len += sceClibSnprintf(line + len, sizeof(line) - len, " 0x%X %.1f/%u/%u;", allowed_callers[i],
                                   site_freed_bytes[i] / 1048576.0f, site_freed[i], site_relocks[i]);
    l_info("%s", line);
}

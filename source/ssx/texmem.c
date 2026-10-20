/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  texmem.c
 * @brief Frees the RAM copy the engine keeps of every texture.
 *
 * RNA::TexturePlatC::CreateInternal copies the pixels of a texture created
 * from memory into a buffer from the "Render" heaps (this+64) and keeps it for
 * the texture's whole life, so that RecreateOnDeviceReset can upload it again
 * after Android loses the GL context. The Vita never loses it, and the copies
 * filled RENDER_GEN (the main menu alone used ~39 of its 48 MB).
 *
 * Once a texture created with pixel data has been uploaded (Flush with a GL
 * name), its copy is freed. Textures created without data (render targets,
 * glyph caches, anything the game fills itself) and file-backed textures
 * (which Flush re-reads from their file) are left alone. Later Flushes of a
 * texture whose copy was freed are skipped: the GPU already has its pixels. If the game ever
 * locks a texture whose copy was freed, SurfacePlatC::Lock gets a new zeroed
 * buffer and the texture is logged, so it can be excluded.
 */

#include "ssx/texmem.h"

#include <stdint.h>
#include <string.h>

#include <psp2/kernel/threadmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

// Offsets in libgame.so v0.0.8430 (local symbols)
#define OFF_TexturePlatC_Flush              0xdbb988 // RNA::TexturePlatC::Flush()
#define OFF_TexturePlatC_CreateInternal     0xdbbc08 // RNA::TexturePlatC::CreateInternal(char*)
#define OFF_TexturePlatC_Destroy            0xdc2960 // RNA::TexturePlatC::Destroy()
#define OFF_TexturePlatC_GetTextureDataSize 0xdbb550 // RNA::TexturePlatC::GetTextureDataSize()
#define OFF_SurfacePlatC_Lock               0xdbb230 // RNA::SurfacePlatC::Lock(unsigned&, unsigned)
// CreateInternal finds its allocator through "ldr r3, [pc, #220]; add r3, pc":
// the global block is at (0xdbbcb0 + 8) + the literal at 0xdbbd8c
#define OFF_RenderGlobalsLiteral            0xdbbd8c
#define OFF_RenderGlobalsBase               (0xdbbcb0 + 8)

// RNA::TexturePlatC fields, as used by the functions above
#define TEX_GL_NAME(t)    (*(uint32_t *)((uint8_t *)(t) + 60))
#define TEX_DATA(t)       (*(uint8_t **)((uint8_t *)(t) + 64))
#define TEX_PATH_BEGIN(t) (*(char **)((uint8_t *)(t) + 120)) // eastl::string
#define TEX_PATH_END(t)   (*(char **)((uint8_t *)(t) + 124))
#define TEX_HAS_PATH(t)   (TEX_PATH_BEGIN(t) != TEX_PATH_END(t))
// RNA::SurfacePlatC: the texture it belongs to
#define SURF_TEXTURE(s)   (*(void **)((uint8_t *)(s) + 36))

// EA::Allocator::ICoreAllocator, as CreateInternal and Destroy call it:
// Alloc(size, name, flags) is slot 2, Free(block, size) is slot 4. The data
// pointer is block + 16, and the block starts with the data size.
typedef void *(*AllocFn)(void *self, uint32_t size, const char *name, uint32_t flags);
typedef void (*FreeFn)(void *self, void *block, uint32_t size);

static int (*Flush_orig)(void *tex);
static int (*CreateInternal_orig)(void *tex, char *data);
static void (*Destroy_orig)(void *tex);
static uint8_t *(*Lock_orig)(void *surf, uint32_t *pitch, uint32_t level);
static uint32_t (*GetTextureDataSize)(void *tex);
static uint8_t *render_globals;

// Textures created with data whose first upload hasn't happened yet
#define MAX_PENDING 1024
static void *pending[MAX_PENDING];
static SceKernelLwMutexWork pending_lock;

static volatile uint32_t freed_bytes, freed_count, relocked_count, skipped_flushes;

static void *render_allocator(void) {
    void *a = *(void **)(render_globals + 108);
    return a ? a : *(void **)(render_globals + 20);
}

static int pending_take(void *tex) {
    int found = 0;
    sceKernelLockLwMutex(&pending_lock, 1, NULL);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (pending[i] == tex) {
            pending[i] = NULL;
            found = 1;
            break;
        }
    }
    sceKernelUnlockLwMutex(&pending_lock, 1);
    return found;
}

static void pending_add(void *tex) {
    sceKernelLockLwMutex(&pending_lock, 1, NULL);
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!pending[i]) {
            pending[i] = tex;
            break;
        }
    }
    sceKernelUnlockLwMutex(&pending_lock, 1);
}

static void free_copy(void *tex) {
    uint8_t *data = TEX_DATA(tex);
    void *alloc = render_allocator();
    if (!data || !alloc || TEX_HAS_PATH(tex) || !TEX_GL_NAME(tex))
        return;

    uint8_t *block = data - 16;
    uint32_t size = *(uint32_t *)block;
    TEX_DATA(tex) = NULL;
    ((FreeFn)(*(void ***)alloc)[4])(alloc, block, 0);

    uint32_t total = __atomic_add_fetch(&freed_bytes, size, __ATOMIC_RELAXED);
    __atomic_add_fetch(&freed_count, 1, __ATOMIC_RELAXED);
    // A line every 8 MB
    if ((total >> 23) != ((total - size) >> 23))
        l_info("[TEXMEM] freed %u texture copies, %.1f MB", freed_count, total / (1024.0f * 1024.0f));
}

static int CreateInternal_hook(void *tex, char *data) {
    int ret = CreateInternal_orig(tex, data);
    if (data && TEX_DATA(tex) && !TEX_HAS_PATH(tex)) {
        if (TEX_GL_NAME(tex))
            free_copy(tex); // uploaded right away (flag 0x10)
        else
            pending_add(tex); // uploaded by the first Bind
    }
    return ret;
}

static int Flush_hook(void *tex) {
    // Flush uploads the pixels again whenever it is called (the terrain
    // textures get flushed again after loading). Without a RAM copy it takes
    // the texture for a file-backed one and maps its (empty) path, so it would
    // upload garbage over the good data the GPU already has.
    if (!TEX_DATA(tex) && !TEX_HAS_PATH(tex) && TEX_GL_NAME(tex)) {
        __atomic_add_fetch(&skipped_flushes, 1, __ATOMIC_RELAXED);
        return 0;
    }
    int ret = Flush_orig(tex);
    if (pending_take(tex))
        free_copy(tex);
    return ret;
}

static void Destroy_hook(void *tex) {
    pending_take(tex);
    Destroy_orig(tex);
}

static uint8_t *Lock_hook(void *surf, uint32_t *pitch, uint32_t level) {
    void *tex = SURF_TEXTURE(surf);
    if (tex && !TEX_DATA(tex) && !TEX_HAS_PATH(tex) && TEX_GL_NAME(tex)) {
        // A texture whose copy was freed: give it a new (zeroed) one
        uint32_t size = GetTextureDataSize(tex);
        void *alloc = render_allocator();
        if (size && alloc) {
            uint8_t *block = ((AllocFn)(*(void ***)alloc)[2])(alloc, size + 16, NULL, 1);
            if (block) {
                *(uint32_t *)block = size;
                memset(block + 16, 0, size);
                TEX_DATA(tex) = block + 16;
                __atomic_add_fetch(&relocked_count, 1, __ATOMIC_RELAXED);
                l_warn("[TEXMEM] texture %p (GL %u, %u bytes) locked after its copy was freed",
                       tex, TEX_GL_NAME(tex), size);
            }
        }
    }
    return Lock_orig(surf, pitch, level);
}

void ssx_texmem_patch(void) {
    sceKernelCreateLwMutex(&pending_lock, "texmem_pending", 0, 0, NULL);

    uintptr_t base = so_mod.text_base;
    render_globals = (uint8_t *)(base + OFF_RenderGlobalsBase + *(uint32_t *)(base + OFF_RenderGlobalsLiteral));
    GetTextureDataSize = (void *)(base + OFF_TexturePlatC_GetTextureDataSize);

    // CreateInternal last: only it starts freeing, so if any hook before it
    // can't be installed, no copy is ever freed
    Lock_orig = (void *)hook_arm_trampoline(&so_mod, base + OFF_SurfacePlatC_Lock, (uintptr_t)&Lock_hook);
    Destroy_orig = (void *)hook_arm_trampoline(&so_mod, base + OFF_TexturePlatC_Destroy, (uintptr_t)&Destroy_hook);
    Flush_orig = (void *)hook_arm_trampoline(&so_mod, base + OFF_TexturePlatC_Flush, (uintptr_t)&Flush_hook);
    if (!Lock_orig || !Destroy_orig || !Flush_orig) {
        l_error("[TEXMEM] could not hook Lock/Destroy/Flush (%p %p %p), texture copies stay in RAM",
                Lock_orig, Destroy_orig, Flush_orig);
        return;
    }
    CreateInternal_orig = (void *)hook_arm_trampoline(&so_mod, base + OFF_TexturePlatC_CreateInternal, (uintptr_t)&CreateInternal_hook);
    if (!CreateInternal_orig)
        l_error("[TEXMEM] could not hook CreateInternal, texture copies stay in RAM");
}

void ssx_texmem_report(void) {
    l_info("[TEXMEM] %u texture copies freed (%.1f MB), %u re-created by Lock, %u re-uploads skipped",
           freed_count, freed_bytes / (1024.0f * 1024.0f), relocked_count, skipped_flushes);
}

/*
 * SSX port: Scaleform mesh cache uploads
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  sfmesh.c
 * @brief Uploads only the changed parts of Scaleform's mesh cache buffers.
 *
 * Without glMapBuffer support (it is only used with GL fences, which vitaGL
 * lacks), Scaleform's GL mesh cache keeps a RAM copy of each of its buffers
 * (1.7 MB of vertices, 1.4 MB of indices) and uploads the whole copy with
 * glBufferSubData when MeshCache::UnlockBuffers unmaps it: nearly every frame
 * in a race, for 4 to 9 ms. Meshes are only written into space handed out by
 * MeshCache::allocBuffer, so the pages it hands out are recorded per buffer,
 * and the upload in MeshBuffer::Unmap goes to vglSsxBufferSubDataPages with
 * them, which copies just those pages (vitaGL checks the rest of the content
 * a piece at a time and repairs it if some change was missed). Comparing the
 * whole upload with a copy instead cost more than the copy it saved.
 */

#include "ssx/sfmesh.h"

#include <stdint.h>

#include <psp2/kernel/clib.h>

#include <so_util/so_util.h>
#include <vitaGL.h>

#include "utils/glutil.h"
#include "utils/logger.h"

extern so_module so_mod;

// libgame.so v0.0.8430
#define OFF_MeshCache_allocBuffer 0xa650d4 // Scaleform::Render::GL::MeshCache::allocBuffer(unsigned*, MeshBuffer**, MeshBufferSet&, unsigned, bool)
#define OFF_MeshBuffer_Unmap      0xa6416c // Scaleform::Render::GL::MeshBuffer::Unmap()
// Scaleform::Render::GL::MeshBuffer fields
#define MB_SIZE(b)   (*(uint32_t *)((uint8_t *)(b) + 20))
#define MB_MAPPED(b) (*(void **)((uint8_t *)(b) + 24))

#define PAGE_SHIFT 10                   // 1 KB pages
#define MAX_WORDS  128                  // buffers up to 4 MB
#define MAX_TRACKED 32

static struct {
    const void *buf; // MeshBuffer
    uint32_t dirty[MAX_WORDS];
} tracked[MAX_TRACKED];
static int num_tracked;

static const void *unmapping; // MeshBuffer being unmapped (render thread)

static int find(const void *buf, int add) {
    for (int i = 0; i < num_tracked; i++)
        if (tracked[i].buf == buf)
            return i;
    // When full, buffers aren't tracked any more: their uploads stay whole
    if (!add || num_tracked == MAX_TRACKED)
        return -1;
    tracked[num_tracked].buf = buf;
    return num_tracked++;
}

typedef uint32_t (*Fn8)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static Fn8 allocBuffer_orig;
static void (*Unmap_orig)(void *buf);

// a0 this, a1 unsigned *offset, a2 MeshBuffer **buffer, a3 MeshBufferSet&,
// a4 size, a5 waitForCache (on the stack)
static uint32_t allocBuffer_hook(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                                 uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) {
    uint32_t ok = allocBuffer_orig(a0, a1, a2, a3, a4, a5, a6, a7);
    if (ok & 0xFF) {
        const void *buf = *(void **)a2;
        uint32_t offset = *(uint32_t *)a1;
        uint32_t size = (a4 + 15) & ~15u; // allocations are in 16-byte units
        int i = find(buf, 1);
        if (i >= 0 && size) {
            uint32_t first = offset >> PAGE_SHIFT, last = (offset + size - 1) >> PAGE_SHIFT;
            if (last >= MAX_WORDS * 32)
                last = MAX_WORDS * 32 - 1;
            for (uint32_t p = first; p <= last; p++)
                tracked[i].dirty[p >> 5] |= 1u << (p & 31);
        }
    }
    return ok;
}

static void Unmap_hook(void *buf) {
    unmapping = buf;
    Unmap_orig(buf);
    unmapping = NULL;
}

void ssx_sfmesh_buffer_sub_data(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) {
    const void *buf = unmapping;
    if (buf && offset == 0 && data && data == MB_MAPPED(buf) && (uint32_t)size == MB_SIZE(buf) &&
        size <= (MAX_WORDS * 32) << PAGE_SHIFT) {
        int i = find(buf, 0);
        if (i >= 0) {
            vglSsxBufferSubDataPages(target, size, data, tracked[i].dirty, PAGE_SHIFT);
            sceClibMemset(tracked[i].dirty, 0, sizeof(tracked[i].dirty));
            return;
        }
    }
    glBufferSubData_soloader(target, offset, size, data);
}

void ssx_sfmesh_patch(void) {
    uintptr_t base = so_mod.text_base;
    allocBuffer_orig = (Fn8)hook_arm_trampoline(&so_mod, base + OFF_MeshCache_allocBuffer, (uintptr_t)allocBuffer_hook);
    if (!allocBuffer_orig) {
        l_error("[SFMESH] could not hook MeshCache::allocBuffer: whole uploads");
        return;
    }
    Unmap_orig = (void *)hook_arm_trampoline(&so_mod, base + OFF_MeshBuffer_Unmap, (uintptr_t)Unmap_hook);
    if (!Unmap_orig)
        l_error("[SFMESH] could not hook MeshBuffer::Unmap: whole uploads");
    else
        l_info("[SFMESH] uploading changed pages of Scaleform's mesh cache");
}

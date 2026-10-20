/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  vaocache.c
 * @brief Mesh vertex setups kept in vertex array objects.
 *
 * RNA::MeshPlatC::BindMesh sets a mesh's vertex elements up before each
 * draw (per element: the attribute location from the shader's scope,
 * glBindBuffer, glEnableVertexAttribArray, glVertexAttribPointer, then the
 * index buffer) and UnbindMesh undoes it all after: ~100 meshes a frame in a
 * race, ~4 elements each. The game has a vertex array object path, but only
 * for meshes marked static, which none are, and it keeps one per mesh while
 * the attribute locations belong to the shader program (RNA asks GL for them
 * after linking; it never binds them).
 *
 * Here each mesh gets a vertex array object holding the setup of its last
 * bind, with what that setup depended on: each element's attribute location
 * and buffer, the stream strides, the index buffer and the vertex format. A
 * bind that would set the same things up just binds the object; any
 * difference sets it up again inside the object, through the game's own
 * code. Unbind then only unbinds the object (and the array buffer, as the
 * game's unbind leaves it at 0).
 *
 * BindMesh and UnbindMesh are only called in pairs (RNAX::RenderMesh, and
 * MeshRenderBatch_Begin/End), so an object is never left bound for other GL
 * code. A mesh's object is freed with the mesh (~MeshPlatC), on the GL
 * thread. vitaGL gives each vertex array object a pool for constant
 * attribute values (glVertexAttrib*), which RNA doesn't use: kept small.
 */

#include "ssx/vaocache.h"

#include <stdint.h>
#include <string.h>

#include <kubridge.h>
#include <so_util/so_util.h>
#include <vitaGL.h>

#include "ssx/glprof.h"
#include "ssx/perf.h"
#include "utils/logger.h"

extern so_module so_mod;

// libgame.so v0.0.8430
#define OFF_MeshPlatC_BindMesh 0xdc1064    // RNA::MeshPlatC::BindMesh(RNA::DeviceC*)
#define OFF_MeshPlatC_UnbindMesh 0xdb8b28  // RNA::MeshPlatC::UnbindMesh(RNA::DeviceC*)
#define OFF_MeshPlatC_dtor 0xdc262c        // RNA::MeshPlatC::~MeshPlatC() (the deleting one calls it)
#define OFF_RNAX_GetLocation 0xd85938      // RNAX::GetLocation(RNA::DeviceC*, RNA::ElementNameE, short)

// MeshPlatC: vertex format +20, streams (array of StreamPlatC*) +28, indices
// +32, the game's own vertex array object +64 and its "make one" flag +68
#define MESH_FORMAT(m) (*(uint8_t **)((uint8_t *)(m) + 20))
#define MESH_STREAMS(m) (*(uint8_t ***)((uint8_t *)(m) + 28))
#define MESH_INDICES(m) (*(uint8_t **)((uint8_t *)(m) + 32))
#define MESH_GAME_VAO(m) (*(uint32_t *)((uint8_t *)(m) + 64))
#define MESH_GAME_WANTS_VAO(m) (*(uint8_t *)((uint8_t *)(m) + 68))
// VertexFormatC: element count +40, elements (24 bytes) from +44: stream
// index (short) +0, name +4, name index (short) +8
#define FORMAT_NELEMENTS(f) (*(int32_t *)((f) + 40))
#define FORMAT_ELEMENT(f, i) ((f) + 44 + 24 * (i))
#define ELEMENT_STREAM(e) (*(int16_t *)((e) + 0))
#define ELEMENT_NAME(e) (*(uint32_t *)((e) + 4))
#define ELEMENT_INDEX(e) (*(int16_t *)((e) + 8))
// StreamPlatC: stride +20, GL buffer +40; IndicesPlatC: GL buffer +28
#define STREAM_STRIDE(s) (*(int32_t *)((s) + 20))
#define STREAM_BUFFER(s) (*(uint32_t *)((s) + 40))
#define INDICES_BUFFER(i) (*(uint32_t *)((i) + 28))

#define MAX_ELEMENTS 12
#define SLOTS 2048 // power of two: one mesh per slot, the last one bound there
#define FREE_QUEUE 256
#define AUX_ATTRIB_POOL 512 // bytes: vitaGL's per-object constant attribute pool
#define MAIN_ATTRIB_POOL (256 * 1024) // vitaGL's default for the default object

typedef struct {
    const void *mesh;
    GLuint vao;
    const uint8_t *format;
    int n;
    uint32_t indices;
    int32_t loc[MAX_ELEMENTS];
    uint32_t buffer[MAX_ELEMENTS];
    int32_t stride[MAX_ELEMENTS];
} entry_t;

typedef void (*BindFn)(void *mesh, void *dev);
typedef void (*DtorFn)(void *mesh);
typedef int32_t (*GetLocationFn)(void *dev, uint32_t name, int32_t index);

static BindFn BindMesh_orig, UnbindMesh_orig;
static DtorFn Dtor_orig;
static GetLocationFn GetLocation;

static entry_t slots[SLOTS];
static const void *bound_mesh; // the mesh whose object is bound (render thread)
static volatile int lock;      // slots and the free queue (destructors run on any thread)
static GLuint free_queue[FREE_QUEUE];
static int nfree;
static volatile uint32_t hits, builds, plain;

static inline void lock_take(void) {
    while (__atomic_exchange_n(&lock, 1, __ATOMIC_ACQUIRE))
        ;
}
static inline void lock_give(void) {
    __atomic_store_n(&lock, 0, __ATOMIC_RELEASE);
}

static inline uint32_t slot_of(const void *mesh) {
    uintptr_t p = (uintptr_t)mesh;
    return (uint32_t)((p >> 4) ^ (p >> 15)) & (SLOTS - 1);
}

// The setup a bind would make: locations, buffers and strides by element
static int describe(void *mesh, void *dev, entry_t *d) {
    const uint8_t *f = MESH_FORMAT(mesh);
    if (!f)
        return 0;
    int n = FORMAT_NELEMENTS(f);
    if (n < 0 || n > MAX_ELEMENTS)
        return 0;
    uint8_t **streams = MESH_STREAMS(mesh);
    d->format = f;
    d->n = n;
    uint8_t *ind = MESH_INDICES(mesh);
    d->indices = ind ? INDICES_BUFFER(ind) : 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *e = FORMAT_ELEMENT(f, i);
        int32_t loc = GetLocation(dev, ELEMENT_NAME(e), ELEMENT_INDEX(e));
        d->loc[i] = loc;
        if (loc >= 0) {
            uint8_t *s = streams[ELEMENT_STREAM(e)];
            d->buffer[i] = STREAM_BUFFER(s);
            d->stride[i] = STREAM_STRIDE(s);
        } else {
            d->buffer[i] = 0;
            d->stride[i] = 0;
        }
    }
    return 1;
}

static inline int same_setup(const entry_t *a, const entry_t *b) {
    if (a->format != b->format || a->n != b->n || a->indices != b->indices)
        return 0;
    for (int i = 0; i < a->n; i++)
        if (a->loc[i] != b->loc[i] || a->buffer[i] != b->buffer[i] || a->stride[i] != b->stride[i])
            return 0;
    return 1;
}

static void free_pending(void) {
    if (!nfree)
        return;
    lock_take();
    glDeleteVertexArrays(nfree, free_queue);
    nfree = 0;
    lock_give();
}

static void BindMesh_hook(void *mesh, void *dev) {
    if (bound_mesh) { // not unbound (cannot happen with the game's pairs)
        glBindVertexArray(0);
        bound_mesh = NULL;
    }
    if (MESH_GAME_VAO(mesh) || MESH_GAME_WANTS_VAO(mesh) || ssx_ab_off || !glprof_on_render_thread()) {
        BindMesh_orig(mesh, dev);
        return;
    }
    entry_t want;
    if (!describe(mesh, dev, &want)) {
        plain++;
        BindMesh_orig(mesh, dev);
        return;
    }
    free_pending();

    lock_take();
    entry_t *s = &slots[slot_of(mesh)];
    if (s->mesh == mesh && s->vao && same_setup(s, &want)) {
        GLuint vao = s->vao;
        lock_give();
        glBindVertexArray(vao);
        bound_mesh = mesh;
        hits++;
        return;
    }
    // Set it up again (or for the first time, or for another mesh of the slot)
    GLuint vao = s->vao;
    entry_t old = *s;
    s->mesh = NULL; // invalid while it's being set up
    lock_give();
    if (!vao)
        glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    // The previous setup's attributes off (the game's code enables its own)
    for (int i = 0; old.vao && i < old.n; i++)
        if (old.loc[i] >= 0)
            glDisableVertexAttribArray(old.loc[i]);
    BindMesh_orig(mesh, dev); // into the object: the element setup and the index buffer
    lock_take();
    *s = want;
    s->mesh = mesh;
    s->vao = vao;
    lock_give();
    bound_mesh = mesh;
    builds++;
}

static void UnbindMesh_hook(void *mesh, void *dev) {
    if (bound_mesh && bound_mesh == mesh) {
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0); // as the game's unbind leaves it
        bound_mesh = NULL;
        return;
    }
    UnbindMesh_orig(mesh, dev);
}

// ~MeshPlatC (the deleting destructor calls this one): the mesh's object goes
// to the free queue, deleted on the GL thread. A slot taken over by another
// mesh keeps its object for that mesh.
static void Dtor_hook(void *mesh) {
    lock_take();
    entry_t *s = &slots[slot_of(mesh)];
    if (s->mesh == mesh) {
        if (s->vao && nfree < FREE_QUEUE) {
            free_queue[nfree++] = s->vao;
            s->vao = 0;
        }
        s->mesh = NULL; // with a full queue the object stays for the slot's next mesh
    }
    lock_give();
    Dtor_orig(mesh);
}

void ssx_vaocache_report(uint32_t frames) {
    if (frames && BindMesh_orig)
        l_info("[PERF] mesh vertex arrays per frame: %u bound as kept, %u set up, %u not kept", hits / frames,
               builds / frames, plain / frames);
    hits = builds = plain = 0;
}

void ssx_vaocache_patch(void) {
    uintptr_t base = so_mod.text_base;
    GetLocation = (GetLocationFn)(base + OFF_RNAX_GetLocation);
    vglSetVertexAttribPoolSize(MAIN_ATTRIB_POOL, AUX_ATTRIB_POOL);
    Dtor_orig = (DtorFn)hook_arm_trampoline(&so_mod, base + OFF_MeshPlatC_dtor, (uintptr_t)Dtor_hook);
    if (!Dtor_orig) {
        l_error("[VAOCACHE] could not hook ~MeshPlatC: meshes set up at each bind");
        return;
    }
    UnbindMesh_orig = (BindFn)hook_arm_trampoline(&so_mod, base + OFF_MeshPlatC_UnbindMesh, (uintptr_t)UnbindMesh_hook);
    BindMesh_orig = (BindFn)hook_arm_trampoline(&so_mod, base + OFF_MeshPlatC_BindMesh, (uintptr_t)BindMesh_hook);
    if (!UnbindMesh_orig || !BindMesh_orig) {
        l_error("[VAOCACHE] could not hook BindMesh/UnbindMesh");
        BindMesh_orig = NULL;
        return;
    }
    l_info("[VAOCACHE] mesh vertex setups kept in vertex array objects");
}

/*
 * SSX port: GPU skinning
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  hwskin.c
 * @brief Skins the characters on the GPU instead of the CPU.
 *
 * CharacterRenderObj::Init gives every character a CpuSkinningFeatureImpl,
 * which skins its meshes on the CPU in EA::Jobs jobs: ~8 ms per race frame on
 * the render thread alone (jobs it runs when the queue is full), plus the job
 * threads' share, with all three cores busy. The game also has
 * GpuSkinningFeatureImpl (vertex shader skinning with a matrix palette, which
 * the player shaders have variants for: g_aMatrixPalette, BLENDINDICES,
 * BLENDWEIGHT), used by GameObjectRenderObj when its config says
 * "gpuSkinning". Both implement ISkinningFeature with the same virtual
 * functions, and nothing calls the CPU one directly, so Init builds the GPU
 * one instead (GameObjectRenderObj's defaults, except mergeSkeletons: false,
 * see make_gpu_feature; useCpuMem false, forceSkinningVertNum -1).
 *
 * The GPU feature also needs its draw calls bracketed, as
 * GameObjectRenderObj::DispatchMeshes does: GpuLockSkinningRegisters before
 * the meshes, GpuUpdateSkinningRegisters (uploads the character's bone
 * matrices into the shared g_aMatrixPalette) before each
 * MeshRenderObject::RenderImmediate, GpuUnlockSkinningRegisters after.
 * CharacterRenderObj's dispatch functions don't (the CPU feature needs
 * nothing there). They are wrapped here.
 *
 * And the meshes need the GPU-skinned shader techniques: GameObjectRenderObj::
 * CacheMeshData looks up "forward_GpuSkinned%d" and so on (the number is the
 * mesh's bones per vertex, GpuSkinningData::+24), which the player shaders
 * have; CharacterRenderObj::CacheMeshData looks up the plain "forward"...,
 * which skin nothing and read the CPU-skinned position stream, now never
 * written: the characters were invisible. While it runs for a character with
 * the GPU feature, FindTechniqueByName tries the _GpuSkinned name first (6
 * lookups per mesh, meshes in order).
 */

#include "ssx/hwskin.h"

#include <stdint.h>
#include <stdio.h>

#include <kubridge.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

#ifndef SSX_HW_SKINNING
#define SSX_HW_SKINNING 1
#endif

// libgame.so v0.0.8430, in CharacterRenderObj::Init(IUnknown32*, BaseRenderObjInit*)
#define OFF_AllocSize      0xfca5f0 // mov r1, #52: sizeof(CpuSkinningFeatureImpl)
#define OFF_InlineCtor     0xfca600 // CpuSkinningFeatureImpl's constructor, inlined (to 0xfca648)
#define OFF_StoreFeature   0xfca648 // str r7, [r5, #188]: the feature, then its Init()
#define OFF_GpuFeatureCtor 0xfd32c4 // GpuSkinningFeatureImpl::GpuSkinningFeatureImpl(bool, bool, int)
#define GPU_FEATURE_SIZE   56       // what GameObjectRenderObj::InitSkinning allocates for it
#define OFF_GpuFeatureVtbl 0x175d440 // _ZTV22GpuSkinningFeatureImpl (vptr = +8)
#define OFF_DispatchMeshes 0xfcab48  // CharacterRenderObj::DispatchMeshes(IUnknown32*, RenderPass)
#define OFF_DispatchBoard  0xfcae84  // CharacterRenderObj::DispatchBoardMeshForCascadeShadows(IUnknown32*, int)
#define OFF_RenderImmediate 0x5cfec8 // MeshRenderObject::RenderImmediate(IUnknown32*, CachedMeshData&, bool)
// CharacterRenderObj / MeshRenderObject fields
#define CHAR_FEATURE(o)  (*(void ***)((uint8_t *)(o) + 188)) // ISkinningFeature*
#define MESH_DATA(o)     (*(uint8_t **)((uint8_t *)(o) + 36)) // CachedMeshData array
#define MESH_DATA_SIZE   40
// ISkinningFeature virtual functions (byte offsets in the vtable)
#define VT_LOCK   28 // GpuLockSkinningRegisters(IUnknown32*)
#define VT_UPDATE 32 // GpuUpdateSkinningRegisters(IUnknown32*, int mesh)
#define VT_UNLOCK 36 // GpuUnlockSkinningRegisters()
#define OFF_CacheMeshData  0xfcb0ac // CharacterRenderObj::CacheMeshData(IUnknown32*)
#define OFF_FindTechnique  0xdb1f34 // RNA::ShaderC::FindTechniqueByName(char const*) const
#define FEATURE_SKIN_DATA(f) (*(uint8_t **)((uint8_t *)(f) + 16)) // vector<GpuSkinningData> begin
#define SKIN_DATA_SIZE     28
#define SKIN_DATA_BONES(d) (*(int32_t *)((uint8_t *)(d) + 24)) // bones per vertex: _GpuSkinned%d
#define LOOKUPS_PER_MESH   6
// Diagnostics
#define OFF_InjectGPUStreams 0xfd588c // GpuSkinningFeatureImpl::CacheMeshDataInjectGPUStreams(IUnknown32*, vector<MeshSkeletonMapping> const*, vector<CachedMeshData>&)
#define OFF_MeshAddStream    0xdb1344 // RNA::MeshC::AddStream(StreamC*)
#define OFF_MeshSetFormat    0xdb18ec // RNA::MeshC::SetVertexFormat(VertexFormatC*)
#define OFF_SetJointPalette  0x1019820 // GameSkinning::SetJointPaletteData(Vector4 const*, int)

static void *(*gpu_ctor)(void *self, int merge_skeletons, int use_cpu_mem, int force_vert_num);
static int created;

typedef uint64_t (*Fn8)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
static Fn8 DispatchMeshes_orig, DispatchBoard_orig, RenderImmediate_orig;
static uintptr_t gpu_vptr;
static void *dispatching; // CharacterRenderObj whose meshes are being drawn (render thread)

static void **gpu_feature(void *character);
static Fn8 CacheMeshData_orig;
static void *(*FindTechnique_orig)(void *shader, const char *name);
static void **caching; // GPU feature of the character in CacheMeshData (render thread)
static int lookups, missing_logged;

static uint64_t CacheMeshData_hook(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                                   uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) {
    void **f = gpu_feature((void *)a0);
    if (!f)
        return CacheMeshData_orig(a0, a1, a2, a3, a4, a5, a6, a7);
    caching = f;
    lookups = 0;
    uint64_t r = CacheMeshData_orig(a0, a1, a2, a3, a4, a5, a6, a7);
    caching = NULL;
    return r;
}

static void *FindTechnique_hook(void *shader, const char *name) {
    if (!caching || !name)
        return FindTechnique_orig(shader, name);
    int mesh = lookups++ / LOOKUPS_PER_MESH;
    int bones = SKIN_DATA_BONES(FEATURE_SKIN_DATA(caching) + mesh * SKIN_DATA_SIZE);
    if (bones >= 1 && bones <= 4) {
        char gpu_name[64];
        snprintf(gpu_name, sizeof(gpu_name), "%s_GpuSkinned%d", name, bones);
        void *t = FindTechnique_orig(shader, gpu_name);
        if (t)
            return t;
        if (missing_logged++ < 8)
            l_warn("[HWSKIN] no technique %s in a character shader: %s used", gpu_name, name);
    }
    return FindTechnique_orig(shader, name);
}

// Diagnostics (see ssx_hwskin_report)
static Fn8 Inject_orig, AddStream_orig, SetFormat_orig, SetPalette_orig;
static int injecting, inject_calls;
static uint32_t inject_streams, inject_formats;
static volatile uint32_t n_dispatch, n_update, n_palette, n_render;

static uint64_t Inject_hook(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                            uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) {
    uint32_t *meshes = (uint32_t *)a3; // vector<CachedMeshData>: begin, end
    injecting++;
    inject_streams = inject_formats = 0;
    uint64_t r = Inject_orig(a0, a1, a2, a3, a4, a5, a6, a7);
    injecting--;
    if (inject_calls++ < 16)
        l_info("[HWSKIN] InjectGPUStreams(feature %p, mappings %p): %d meshes, %u streams added, %u vertex formats set",
               (void *)a0, (void *)a2, meshes ? (int)((meshes[1] - meshes[0]) / MESH_DATA_SIZE) : -1,
               inject_streams, inject_formats);
    return r;
}
static uint64_t AddStream_hook(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                               uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) {
    if (injecting)
        inject_streams++;
    return AddStream_orig(a0, a1, a2, a3, a4, a5, a6, a7);
}
static uint64_t SetFormat_hook(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                               uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) {
    if (injecting)
        inject_formats++;
    return SetFormat_orig(a0, a1, a2, a3, a4, a5, a6, a7);
}
static uint64_t SetPalette_hook(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                                uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) {
    if (n_palette++ == 0) {
        const float *m = (const float *)a0;
        l_info("[HWSKIN] first palette (%d vec4): %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f",
               (int)a1, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
    }
    return SetPalette_orig(a0, a1, a2, a3, a4, a5, a6, a7);
}

void ssx_hwskin_report(uint32_t frames) {
    if (frames && (n_dispatch || n_update || n_palette))
        l_info("[PERF] GPU skinning per frame: %.1f character dispatches, %.1f meshes drawn, %.1f register updates, "
               "%.1f palette uploads", n_dispatch / (float)frames, n_render / (float)frames,
               n_update / (float)frames, n_palette / (float)frames);
    n_dispatch = n_update = n_render = 0;
    n_palette = n_palette ? 1 : 0;
}

static void **gpu_feature(void *character) {
    void **f = CHAR_FEATURE(character);
    return f && (uintptr_t)f[0] == gpu_vptr ? f : NULL;
}

#define VCALL(f, off, type) ((type)(*(void ***)(f))[(off) / 4])

// Brackets a CharacterRenderObj dispatch function like GameObjectRenderObj's
#define DISPATCH_HOOK(name)                                                                      \
    static uint64_t name##_hook(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,              \
                                uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) {            \
        void **f = gpu_feature((void *)a0);                                                       \
        if (!f)                                                                                   \
            return name##_orig(a0, a1, a2, a3, a4, a5, a6, a7);                                   \
        VCALL(f, VT_LOCK, void (*)(void *, void *))(f, (void *)a1);                               \
        n_dispatch++;                                                                             \
        void *outer = dispatching;                                                                \
        dispatching = (void *)a0;                                                                 \
        uint64_t r = name##_orig(a0, a1, a2, a3, a4, a5, a6, a7);                                 \
        dispatching = outer;                                                                      \
        VCALL(f, VT_UNLOCK, void (*)(void *))(f);                                                 \
        return r;                                                                                 \
    }
DISPATCH_HOOK(DispatchMeshes)
DISPATCH_HOOK(DispatchBoard)

// a0 this, a1 IUnknown32*, a2 CachedMeshData&, a3 bool
static uint64_t RenderImmediate_hook(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
                                     uint32_t a4, uint32_t a5, uint32_t a6, uint32_t a7) {
    if (dispatching && (void *)a0 == dispatching) {
        void **f = gpu_feature((void *)a0);
        int mesh = (int)(((uint8_t *)a2 - MESH_DATA((void *)a0)) / MESH_DATA_SIZE);
        n_render++;
        if (f) {
            VCALL(f, VT_UPDATE, void (*)(void *, void *, int))(f, (void *)a1, mesh);
            n_update++;
        }
    }
    return RenderImmediate_orig(a0, a1, a2, a3, a4, a5, a6, a7);
}

// Called from the patched Init with the allocation in r0; returns it built
static void *make_gpu_feature(void *mem) {
    // mergeSkeletons off: one shared palette holds 50 bones (150 vec4) and a
    // rider has more (fingers, the board): hands and board were drawn with
    // other bones. Unmerged, each mesh uploads its own bones before its draw.
    gpu_ctor(mem, 0, 0, -1);
    if (created++ == 0)
        l_info("[HWSKIN] characters are skinned on the GPU (GpuSkinningFeatureImpl)");
    return mem;
}

static uint32_t arm_b(uintptr_t from, uintptr_t to) {
    return 0xEA000000 | (((to - (from + 8)) >> 2) & 0x00FFFFFF);
}

void ssx_hwskin_patch(void) {
#if SSX_HW_SKINNING
    uintptr_t base = so_mod.text_base;
    uint32_t *size = (uint32_t *)(base + OFF_AllocSize);
    uint32_t *code = (uint32_t *)(base + OFF_InlineCtor);
    if (*size != 0xE3A01034 || code[0] != 0xE59F3294 || code[0x48 / 4] != 0xE58570BC) {
        l_error("[HWSKIN] CharacterRenderObj::Init is not as expected: not patched");
        return;
    }
    gpu_ctor = (void *)(base + OFF_GpuFeatureCtor);
    gpu_vptr = base + OFF_GpuFeatureVtbl + 8;
    DispatchMeshes_orig = (Fn8)hook_arm_trampoline(&so_mod, base + OFF_DispatchMeshes, (uintptr_t)DispatchMeshes_hook);
    DispatchBoard_orig = (Fn8)hook_arm_trampoline(&so_mod, base + OFF_DispatchBoard, (uintptr_t)DispatchBoard_hook);
    RenderImmediate_orig = (Fn8)hook_arm_trampoline(&so_mod, base + OFF_RenderImmediate, (uintptr_t)RenderImmediate_hook);
    CacheMeshData_orig = (Fn8)hook_arm_trampoline(&so_mod, base + OFF_CacheMeshData, (uintptr_t)CacheMeshData_hook);
    FindTechnique_orig = (void *)hook_arm_trampoline(&so_mod, base + OFF_FindTechnique, (uintptr_t)FindTechnique_hook);
    Inject_orig = (Fn8)hook_arm_trampoline(&so_mod, base + OFF_InjectGPUStreams, (uintptr_t)Inject_hook);
    AddStream_orig = (Fn8)hook_arm_trampoline(&so_mod, base + OFF_MeshAddStream, (uintptr_t)AddStream_hook);
    SetFormat_orig = (Fn8)hook_arm_trampoline(&so_mod, base + OFF_MeshSetFormat, (uintptr_t)SetFormat_hook);
    SetPalette_orig = (Fn8)hook_arm_trampoline(&so_mod, base + OFF_SetJointPalette, (uintptr_t)SetPalette_hook);
    if (!Inject_orig || !AddStream_orig || !SetFormat_orig || !SetPalette_orig)
        l_error("[HWSKIN] diagnostics hooks failed");
    if (!DispatchMeshes_orig || !DispatchBoard_orig || !RenderImmediate_orig || !CacheMeshData_orig ||
        !FindTechnique_orig) {
        l_error("[HWSKIN] could not hook the character draw functions: GPU skinning off");
        return;
    }

    *size = 0xE3A01038;                                    // mov r1, #56
    code[0] = 0xE1A07000;                                  // mov r7, r0
    code[1] = 0xE59FC000;                                  // ldr r12, [pc, #0] (code[3])
    code[2] = arm_b((uintptr_t)&code[2], (uintptr_t)&code[4]);
    code[3] = (uint32_t)(uintptr_t)make_gpu_feature;
    code[4] = 0xE1A00007;                                  // mov r0, r7
    code[5] = 0xE12FFF3C;                                  // blx r12
    code[6] = arm_b((uintptr_t)&code[6], base + OFF_StoreFeature);
    kuKernelFlushCaches(size, 4);
    kuKernelFlushCaches(code, 7 * 4);
#endif
}

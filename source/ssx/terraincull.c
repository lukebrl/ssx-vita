/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  terraincull.c
 * @brief Terrain chunks outside the view are not drawn.
 *
 * TerrainRenderObj::Render and SortObjectsForDispatch collect the chunks to
 * draw from all the loaded terrain objects: a chunk is kept when its
 * distance (a Bvm2 result) is under a limit and SelectLOD gives it a level.
 * EA::CoreWorld::Bvm2 tests each chunk's box against the frustum and stores
 * a class per chunk (0xC0 outside), but it writes the distance and LOD
 * values only for the chunks inside: the others keep the values of the last
 * frame they were in view, and the collect loops never read the class. In
 * the races every loaded chunk was drawn, about half of them outside the
 * view (60 of 104 on the big maps), each one a material bind and a draw.
 *
 * SelectLOD (called only by these two loops) is replaced by the same code
 * that first returns "no level" for a chunk classed outside. When all the
 * chunks are classed outside the results are taken as not computed for this
 * view (seen while a race starts) and nothing is culled.
 *
 * Debug builds with SSX_DIAG count the chunks collected and those outside
 * ([TERRAIN]).
 */

#include "ssx/terraincull.h"

#include <math.h>
#include <stdint.h>

#include <kubridge.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

// libgame.so v0.0.8430
#define OFF_TerrainRenderObj_Render 0xffbd38 // TerrainRenderObj::Render(IUnknown32*, unsigned, double)
#define OFF_SortObjectsForDispatch 0xffb7c0  // TerrainRenderObj::SortObjectsForDispatch(int, unsigned)
#define OFF_SelectLOD 0xff5a58               // TerrainGameObject::SelectLOD(float) const
#define OFF_HandleMap_Index 0xff968c         // eastl::map<TerrainGameObject const*, Bvm2::BvmHandle>::operator[]

// TerrainRenderObj: collected chunks (SortableChunk, 24 bytes, object entry
// at +16) +40..+44, terrain object entries (256 bytes, object first) +80..+84,
// Bvm2 +228, handle map +232
#define TRO_CHUNKS(t) (*(uint8_t **)((uint8_t *)(t) + 40))
#define TRO_CHUNKS_END(t) (*(uint8_t **)((uint8_t *)(t) + 44))
#define TRO_OBJECTS(t) (*(uint8_t **)((uint8_t *)(t) + 80))
#define TRO_OBJECTS_END(t) (*(uint8_t **)((uint8_t *)(t) + 84))
#define TRO_BVM(t) (*(uint8_t **)((uint8_t *)(t) + 228))
#define TRO_MAP(t) ((uint8_t *)(t) + 232)
#define CHUNK_SIZE 24
#define OBJECT_SIZE 256
// Bvm2 frustum results (FrustumResultData at +7072): class per handle +7536
#define BVM_CLASS(b) (*(uint8_t **)((b) + 7536))
#define CLASS_OUTSIDE 0xC0
// TerrainGameObject: LOD count +16, LODs (32 bytes, switch value at +24) +12
#define OBJ_NLODS(o) (*(int32_t *)((uint8_t *)(o) + 16))
#define OBJ_LODS(o) (*(uint8_t **)((uint8_t *)(o) + 12))
#define LOD_SIZE 32
// Render collects (and calls SelectLOD) in these passes only
#define PASS_SHAD 0x53484144
#define PASS_GBUF 0x47425546

typedef uint16_t *(*MapIndexFn)(void *map, void *const *key);

static void (*Render_orig)(void *, void *, uint32_t, double);
static void (*Sort_orig)(void *, int, uint32_t);
static MapIndexFn MapIndex;

// The TerrainRenderObj collecting, when its Bvm2 results can be used
static void *culling;

static int chunk_class(void *tro, const void *obj) {
    uint16_t h = *MapIndex(TRO_MAP(tro), (void *const *)&obj);
    return BVM_CLASS(TRO_BVM(tro))[h >> 2];
}

// Culls with these Bvm2 results unless they class every chunk outside
static void *cull_with(void *tro) {
    if (!TRO_BVM(tro) || !BVM_CLASS(TRO_BVM(tro)))
        return NULL;
    for (uint8_t *o = TRO_OBJECTS(tro); o && o < TRO_OBJECTS_END(tro); o += OBJECT_SIZE)
        if (chunk_class(tro, *(void **)o) != CLASS_OUTSIDE)
            return tro;
    return NULL;
}

// The game's SelectLOD: the first level whose switch value is at most the
// metric (negative metrics read 0), -1 for none
static int SelectLOD(const void *obj, float metric) {
    if (culling && chunk_class(culling, obj) == CLASS_OUTSIDE)
        return -1;
    if (metric < 0.0f)
        metric = 0.0f;
    const uint8_t *lod = OBJ_LODS(obj);
    for (int i = 0, n = OBJ_NLODS(obj); i < n; i++, lod += LOD_SIZE)
        if (*(const float *)(lod + 24) <= metric)
            return i;
    return -1;
}

static void Sort_hook(void *tro, int view, uint32_t pass) {
    culling = cull_with(tro);
    Sort_orig(tro, view, pass);
    culling = NULL;
}

#ifdef DEBUG_SOLOADER
#define REPORT_US 5000000
#define NPASSES 8

static struct {
    uint32_t frames, objects, objects_out, drawn, drawn_out, uncullable;
    struct {
        uint32_t id, calls, us, chunks;
    } pass[NPASSES]; // Render calls by pass: time with callees, chunks collected
} st;

// The far plane of the projection terrain is drawn with: Bvm2's frustum has
// it at 1e6 (GameSpecificRender::ComputeVisibility calls SetFarClip), so
// chunks past it are drawn and clipped whole by the GPU. An EA Frustum built
// from the device's view and projection: far plane (normal, d) at +16, eye
// at +368. Chunk boxes (center, half size) at bvm+7036, 32 bytes a handle.
#define OFF_Frustum_ctor 0x5c29f0
#define OFF_Frustum_dtor 0x5bfd28
#define OFF_Frustum_SetViewProj 0x5c5088
#define OFF_Device_GetView 0xdafd20
#define OFF_Device_GetProjection 0xdafc94
#define IID_DeviceC 0x092398ad
#define BVM_BOXES(b) (*(float **)((b) + 7036))

static struct {
    uint32_t chunks, beyond, near_far; // drawn chunks: all, wholly past the far plane, within 10% of it
    float far_min, far_max, dist_max;  // eye to far plane; farthest chunk center
} fs;

static void far_test(void *tro, void *ctx) {
    if (!ctx)
        return;
    uintptr_t base = so_mod.text_base;
    void *dev = ((void *(*)(void *, uint32_t))(*(void ***)ctx)[4])(ctx, IID_DeviceC);
    if (!dev)
        return;
    float view[16], proj[16];
    static uint8_t mem[1024] __attribute__((aligned(16)));
    ((void (*)(float *, void *))(base + OFF_Device_GetView))(view, dev);
    ((void (*)(float *, void *))(base + OFF_Device_GetProjection))(proj, dev);
    ((void (*)(void *))(base + OFF_Frustum_ctor))(mem);
    ((void (*)(void *, const float *, const float *))(base + OFF_Frustum_SetViewProj))(mem, view, proj);
    const float *pl = (const float *)(mem + 16), *eye = (const float *)(mem + 368);
    float len = __builtin_sqrtf(pl[0] * pl[0] + pl[1] * pl[1] + pl[2] * pl[2]);
    float at_eye = pl[0] * eye[0] + pl[1] * eye[1] + pl[2] * eye[2] + pl[3];
    float s = at_eye < 0 ? -1.0f : 1.0f; // the side the eye is on is inside
    float far_dist = s * at_eye / len;
    if (far_dist < fs.far_min || !fs.far_min)
        fs.far_min = far_dist;
    if (far_dist > fs.far_max)
        fs.far_max = far_dist;
    const float *boxes = BVM_BOXES(TRO_BVM(tro));
    for (uint8_t *c = TRO_CHUNKS(tro); boxes && c && c < TRO_CHUNKS_END(tro); c += CHUNK_SIZE) {
        const void *obj = **(void ***)(c + 16);
        uint16_t h = *MapIndex(TRO_MAP(tro), (void *const *)&obj);
        const float *b = boxes + (h >> 2) * 8; // center x y z w, half size x y z w
        float e = s * (pl[0] * b[0] + pl[1] * b[1] + pl[2] * b[2] + pl[3]);
        float r = __builtin_fabsf(pl[0]) * b[4] + __builtin_fabsf(pl[1]) * b[5] + __builtin_fabsf(pl[2]) * b[6];
        float dx = b[0] - eye[0], dy = b[1] - eye[1], dz = b[2] - eye[2];
        float d = __builtin_sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > fs.dist_max)
            fs.dist_max = d;
        fs.chunks++;
        if (e + r < 0)
            fs.beyond++;
        else if ((e - r) / len < 0.1f * far_dist)
            fs.near_far++;
    }
    ((void (*)(void *))(base + OFF_Frustum_dtor))(mem);
}

static void count(void *tro, void *ctx, int culled) {
    if (!TRO_BVM(tro) || !BVM_CLASS(TRO_BVM(tro)))
        return;
    st.frames++;
    st.uncullable += !culled;
    for (uint8_t *o = TRO_OBJECTS(tro); o && o < TRO_OBJECTS_END(tro); o += OBJECT_SIZE) {
        st.objects++;
        st.objects_out += chunk_class(tro, *(void **)o) == CLASS_OUTSIDE;
    }
    for (uint8_t *c = TRO_CHUNKS(tro); c && c < TRO_CHUNKS_END(tro); c += CHUNK_SIZE) {
        st.drawn++;
        st.drawn_out += chunk_class(tro, **(void ***)(c + 16)) == CLASS_OUTSIDE;
    }
    far_test(tro, ctx);
}

static void count_pass(void *tro, uint32_t pass, uint32_t us, int collected) {
    int i = 0;
    while (i < NPASSES - 1 && st.pass[i].id && st.pass[i].id != pass)
        i++;
    st.pass[i].id = pass;
    st.pass[i].calls++;
    st.pass[i].us += us;
    if (collected)
        st.pass[i].chunks += (TRO_CHUNKS_END(tro) - TRO_CHUNKS(tro)) / CHUNK_SIZE;

    static uint32_t last;
    uint32_t now = sceKernelGetProcessTimeLow();
    if (now - last < REPORT_US)
        return;
    uint32_t secs = (now - last) / 1000000;
    last = now;
    uint32_t f = st.frames ? st.frames : 1;
    l_info("[TERRAIN] per collect: %u objects (%u outside the view), %u chunks drawn, %u of them outside; "
           "%u of %u collects not culled",
           st.objects / f, st.objects_out / f, st.drawn / f, st.drawn_out / f, st.uncullable, st.frames);
    char line[512];
    int n = 0;
    for (int k = 0; k < NPASSES && st.pass[k].id && n < (int)sizeof(line); k++) {
        uint32_t id = st.pass[k].id, calls = st.pass[k].calls;
        n += sceClibSnprintf(line + n, sizeof(line) - n, " %c%c%c%c: %u a second, %u us each, %u chunks;",
                             (int)(id >> 24), (int)(id >> 16 & 0xff), (int)(id >> 8 & 0xff), (int)(id & 0xff),
                             calls / (secs ? secs : 1), calls ? st.pass[k].us / calls : 0,
                             calls ? st.pass[k].chunks / calls : 0);
    }
    l_info("[TERRAIN] Render by pass:%s", n ? line : " none");
    // Render's own limit on the Bvm2 distance: 600 / cos(half the angle at bvm+7472)
    float fov = TRO_BVM(tro) ? *(float *)(TRO_BVM(tro) + 7472) : 0.0f;
    l_info("[TERRAIN] far plane %.0f-%.0f from the eye (Bvm2: 1e6), collect limit %.0f (angle %.2f); drawn chunks per "
           "collect: %u, %u wholly past the far plane, %u within 10%% of it; farthest chunk center %.0f",
           fs.far_min, fs.far_max, 600.0f / cosf(0.5f * fov), fov, fs.chunks / f, fs.beyond / f, fs.near_far / f,
           fs.dist_max);
    fs = (typeof(fs)){0};
    st = (typeof(st)){0};
}
#endif

static void Render_hook(void *tro, void *ctx, uint32_t pass, double t) {
#ifdef DEBUG_SOLOADER
    uint32_t t0 = sceKernelGetProcessTimeLow();
#endif
    int collect = pass == PASS_SHAD || pass == PASS_GBUF;
    culling = collect ? cull_with(tro) : NULL;
#ifdef DEBUG_SOLOADER
    int culled = culling != NULL;
#endif
    Render_orig(tro, ctx, pass, t);
    culling = NULL;
#ifdef DEBUG_SOLOADER
    if (SSX_DIAG) {
        if (collect)
            count(tro, ctx, culled);
        count_pass(tro, pass, sceKernelGetProcessTimeLow() - t0, collect);
    }
#endif
}

void ssx_terraincull_patch(void) {
    uintptr_t base = so_mod.text_base;
    MapIndex = (MapIndexFn)(base + OFF_HandleMap_Index);
    Render_orig = (void *)hook_arm_trampoline(&so_mod, base + OFF_TerrainRenderObj_Render, (uintptr_t)Render_hook);
    Sort_orig = (void *)hook_arm_trampoline(&so_mod, base + OFF_SortObjectsForDispatch, (uintptr_t)Sort_hook);
    if (!Render_orig || !Sort_orig) {
        l_error("[TERRAIN] could not hook the terrain collect, chunks outside the view still drawn");
        return;
    }
    hook_addr(base + OFF_SelectLOD, (uintptr_t)&SelectLOD);
    l_info("[TERRAIN] chunks outside the view not drawn");
}

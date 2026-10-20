/*
 * SSX Vita port
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

/**
 * @file  terrainmerge.c
 * @brief Draws the terrain chunks of a bin with a short per-draw path.
 *
 * TerrainRenderObj::DispatchDeferredDispatchBin draws a bin's terrain chunks
 * one at a time: for each, it points the chunk's mesh at the chunk's index
 * range (MeshC::SetPrimRange) and renders it with
 * RNAX::MeshRenderImmediateMultipass, which runs the whole per-draw path of
 * RNAX::RenderMesh for every pass: SetPass, SetVertexFormat, scope
 * FlushMappings, the shader's parameter upload, render and sampler state,
 * BindMesh, the draw, UnbindMesh. Terrain is 150-320 draws per race frame,
 * most of the frame's draws, on the render thread (the critical path).
 *
 * Every chunk has its own mesh (its own vertex and index buffers), so they
 * can't be merged into fewer draws. But between two chunks of a bin nothing
 * changes except that mesh: same material, shader, parameters and state. So
 * the first chunk of a bin takes the full path, and the next ones whose
 * vertex format is the same object, with a single pass and 16-bit indexed
 * triangles, only bind their buffers, draw their range and unbind, as
 * RenderMesh does in its loop. Anything else takes the full path.
 *
 * The function is replaced whole (both copies: the regular one and a
 * constant-propagated one with its own LOD table).
 */

#include "ssx/terrainmerge.h"

#include <stdint.h>

#include <kubridge.h>
#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>
#include <so_util/so_util.h>

#include "utils/logger.h"

extern so_module so_mod;

// libgame.so v0.0.8430
#define OFF_SetPrimRange 0xdb1998                 // RNA::MeshC::SetPrimRange(int, int)
#define OFF_MeshRenderImmediateMultipass 0xd85e58 // RNAX::MeshRenderImmediateMultipass(DeviceC*, void*, MeshC*, MaterialC*)
#define OFF_GetOGLES20 0xdb7b78                   // RNA::DeviceC::GetOGLES20() const
#define OFF_GetNumPasses 0xdafb68                 // RNA::DeviceC::GetNumPasses() const
#define OFF_UnbindMesh 0xdb8b28                   // RNA::MeshPlatC::UnbindMesh(DeviceC*)
static const struct {
    uint32_t func;          // TerrainRenderObj::DispatchDeferredDispatchBin
    uint32_t table_insn;    // "add r5, pc, r5" giving its LOD table...
    uint32_t table_literal; // ...from this literal
} variants[] = {
    { 0xff88bc, 0xff88cc, 0xff89c4 },
    { 0xff7a24, 0xff7a34, 0xff7b2c }, // .constprop.93
};
#define NVARIANTS (sizeof(variants) / sizeof(variants[0]))

// MeshC / MeshPlatC fields
#define MESH_PRIM_TYPE(m)   (*(uint8_t *)((uint8_t *)(m) + 18))
#define MESH_FORMAT(m)      (*(void **)((uint8_t *)(m) + 20))    // VertexFormatC*
#define MESH_INDICES(m)     (*(uint8_t **)((uint8_t *)(m) + 32)) // IndicesC*, NULL if not indexed
#define MESH_RANGE_START(m) (*(int32_t *)((uint8_t *)(m) + 36))  // in indices
#define MESH_RANGE_COUNT(m) (*(int32_t *)((uint8_t *)(m) + 40))  // in primitives
#define INDICES_32BIT(i)    (*(uint32_t *)((i) + 48))
#define PRIM_TRIANGLES 4
#define LOD_FULL 3 // ShaderLOD drawn without ranges
#define GL_TRIANGLES_ 0x0004
#define GL_UNSIGNED_SHORT_ 0x1403
#define OGLES20_DRAW_ELEMENTS 43 // EA::Graphics::OpenGLES20::glDrawElements vtable slot

typedef void (*DrawElementsFn)(void *self, uint32_t mode, int32_t count, uint32_t type, const void *indices);

static void (*SetPrimRange)(void *mesh, int start, int count);
static void (*MeshRenderImmediateMultipass)(void *device, void *unused, void *mesh, void *material);
static void *(*GetOGLES20)(void *device);
static int (*GetNumPasses)(void *device);
static void (*UnbindMesh)(void *mesh, void *device);
static const int32_t *lod_tables[NVARIANTS];

static volatile uint32_t chunks, short_draws, full_draws;
// Time per step, in microseconds (debug aid)
static uint32_t t_full, t_bind, t_draw, t_unbind;

#define NOW() sceKernelGetProcessTimeLow()

static void draw_full(void *device, void *mesh, int start, int count) {
    uint32_t t0 = NOW();
    int old_start = MESH_RANGE_START(mesh), old_count = MESH_RANGE_COUNT(mesh);
    SetPrimRange(mesh, start, count);
    MeshRenderImmediateMultipass(device, NULL, mesh, NULL);
    SetPrimRange(mesh, old_start, old_count);
    t_full += NOW() - t0;
    full_draws++;
}

// --- The short path -------------------------------------------------------
//
// RNA::MeshPlatC::BindMesh, for each element of the mesh's vertex format:
// looks up the attribute location for the element (RNAX::GetLocation, a scope
// lookup), binds the element's stream buffer, enables the attribute and sets
// its pointer; then binds the index buffer. UnbindMesh looks the locations
// up again, unbinds and disables. Over a run of chunks with the same format
// and program, the locations and the per-element layout don't change and
// the attributes can stay enabled: only the stream buffers, the pointers
// (same layout, other buffer) and the index buffer change per chunk. The run
// ends with the game's UnbindMesh, which leaves things as the game expects.

#define OFF_GetLocation 0xd85938 // RNAX::GetLocation(DeviceC*, ElementName, short)
// BindMesh's tables, from the "add rX, pc, rX" at these addresses and their literals
#define OFF_SIZES_INSN 0xdc10a8 // components per element type, 12-byte entries, +4
#define OFF_SIZES_LIT 0xdc1278
#define OFF_TYPES_INSN 0xdc10c0 // GL types of element types 1-45, halfwords at +832
#define OFF_TYPES_LIT 0xdc1280
#define OFF_NORMS_INSN 0xdc10b4 // normalized flags of element types 9-45, bytes at +792
#define OFF_NORMS_LIT 0xdc127c

// VertexFormatC: element count at +40, 24-byte elements from +44
#define FORMAT_COUNT(f) (*(int32_t *)((uint8_t *)(f) + 40))
#define FORMAT_ELEM(f, i) ((uint8_t *)(f) + 44 + 24 * (i))
#define ELEM_STREAM(e) (*(int16_t *)((e) + 0))
#define ELEM_NAME(e) (*(int32_t *)((e) + 4))
#define ELEM_USAGE(e) (*(int16_t *)((e) + 8))
#define ELEM_TYPE(e) (*(int32_t *)((e) + 12))
#define ELEM_OFFSET(e) (*(uint16_t *)((e) + 16))
// MeshPlatC: streams (StreamPlatC*[]) at +28, VAO at +64, "use a VAO" at +68
#define MESH_STREAMS(m) (*(uint8_t ***)((uint8_t *)(m) + 28))
#define MESH_VAO(m) (*(uint32_t *)((uint8_t *)(m) + 64))
#define MESH_USE_VAO(m) (*(uint8_t *)((uint8_t *)(m) + 68))
// StreamPlatC: stride at +20, device at +36, GL name at +40; IndicesC: device +24, name +28
#define STREAM_STRIDE(s) (*(int32_t *)((s) + 20))
#define STREAM_DEVICE(s) (*(void **)((s) + 36))
#define STREAM_NAME(s) (*(uint32_t *)((s) + 40))
#define INDICES_DEVICE(i) (*(void **)((i) + 24))
#define INDICES_NAME(i) (*(uint32_t *)((i) + 28))
// Vtable slots: DevicePlatC::BindBuffer(target, name); OpenGLES20 functions
#define DEVICE_BIND_BUFFER 5
#define OGLES20_ENABLE_ATTRIB 45
#define OGLES20_ATTRIB_POINTER 142
#define GL_ARRAY_BUFFER_ 0x8892
#define GL_ELEMENT_ARRAY_BUFFER_ 0x8893
#define GL_FLOAT_ 0x1406
#define MAX_ELEMS 16

typedef void (*BindBufferFn)(void *self, uint32_t target, uint32_t name);
typedef void (*EnableAttribFn)(void *self, int32_t loc);
typedef void (*AttribPointerFn)(void *self, int32_t loc, int32_t size, uint32_t type, uint32_t normalized,
                                int32_t stride, uint32_t offset);

static int32_t (*GetLocation)(void *device, int32_t name, int32_t usage);
static const uint8_t *sizes_table, *types_table, *norms_table;

static struct {
    void *format;    // NULL: no run
    void *last_mesh; // for the UnbindMesh that ends the run
    int n;
    struct {
        int32_t loc, size, stream;
        uint32_t type, normalized, offset;
    } e[MAX_ELEMS];
} run;

static void elem_layout(const uint8_t *e, int32_t *size, uint32_t *type, uint32_t *normalized) {
    uint32_t t = (uint32_t)ELEM_TYPE(e);
    *size = *(const int32_t *)(sizes_table + t * 12 + 4);
    *type = t - 1 <= 44 ? *(const uint16_t *)(types_table + (t - 1) * 2 + 832) : GL_FLOAT_;
    *normalized = t - 9 <= 36 ? *(const uint8_t *)(norms_table + (t - 9) + 792) : 0;
}

// Starts a run for this format, with the locations of the current program
static int run_start(void *device, void *format) {
    int n = FORMAT_COUNT(format);
    if (n <= 0 || n > MAX_ELEMS)
        return 0;
    void *ogl = GetOGLES20(device);
    run.n = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *e = FORMAT_ELEM(format, i);
        int32_t loc = GetLocation(device, ELEM_NAME(e), ELEM_USAGE(e));
        if (loc < 0)
            continue;
        int k = run.n++;
        run.e[k].loc = loc;
        run.e[k].stream = ELEM_STREAM(e);
        run.e[k].offset = ELEM_OFFSET(e);
        elem_layout(e, &run.e[k].size, &run.e[k].type, &run.e[k].normalized);
        ((EnableAttribFn)(*(void ***)ogl)[OGLES20_ENABLE_ATTRIB])(ogl, loc);
    }
    run.format = format;
    run.last_mesh = NULL;
    return 1;
}

static void run_end(void *device) {
    if (!run.format)
        return;
    uint32_t t0 = NOW();
    if (run.last_mesh)
        UnbindMesh(run.last_mesh, device);
    t_unbind += NOW() - t0;
    run.format = NULL;
}

static void draw_short(void *device, void *mesh, int start, int count) {
    uint32_t t0 = NOW();
    void *ogl = GetOGLES20(device);
    uint8_t **streams = MESH_STREAMS(mesh);
    for (int k = 0; k < run.n; k++) {
        uint8_t *st = streams[run.e[k].stream];
        void *sdev = STREAM_DEVICE(st);
        ((BindBufferFn)(*(void ***)sdev)[DEVICE_BIND_BUFFER])(sdev, GL_ARRAY_BUFFER_, STREAM_NAME(st));
        ((AttribPointerFn)(*(void ***)ogl)[OGLES20_ATTRIB_POINTER])(ogl, run.e[k].loc, run.e[k].size, run.e[k].type,
                                                                   run.e[k].normalized, STREAM_STRIDE(st),
                                                                   run.e[k].offset);
    }
    uint8_t *indices = MESH_INDICES(mesh);
    void *idev = INDICES_DEVICE(indices);
    ((BindBufferFn)(*(void ***)idev)[DEVICE_BIND_BUFFER])(idev, GL_ELEMENT_ARRAY_BUFFER_, INDICES_NAME(indices));
    uint32_t t1 = NOW();
    ((DrawElementsFn)(*(void ***)ogl)[OGLES20_DRAW_ELEMENTS])(ogl, GL_TRIANGLES_, count * 3, GL_UNSIGNED_SHORT_,
                                                             (const void *)(uintptr_t)(start * 2));
    t_bind += t1 - t0;
    t_draw += NOW() - t1;
    run.last_mesh = mesh;
    short_draws++;
}

static int can_draw_short(void *device, void *mesh, void *format) {
    uint8_t *indices = MESH_INDICES(mesh);
    return format && MESH_FORMAT(mesh) == format && MESH_PRIM_TYPE(mesh) == PRIM_TRIANGLES && indices &&
           !INDICES_32BIT(indices) && !MESH_VAO(mesh) && !MESH_USE_VAO(mesh) && GetNumPasses(device) == 1;
}

// A bin entry is 24 bytes: +16 the terrain object (its first word points at
// data whose +12 is the chunk array), +20 the chunk index. A chunk is 32
// bytes: a (start, count) pair per LOD, and the mesh at +28.
static uint8_t *chunk_of(const uint8_t *entry) {
    const uint8_t *obj = *(const uint8_t **)(entry + 16);
    const uint8_t *data = *(const uint8_t **)obj;
    return *(uint8_t **)(data + 12) + *(const int32_t *)(entry + 20) * 32;
}

static void dispatch_bin(int variant, void *device, uint8_t *bin, int lod) {
    if (lod == LOD_FULL) {
        for (uint8_t *e = *(uint8_t **)(bin + 12); e != *(uint8_t **)(bin + 16); e += 24) {
            MeshRenderImmediateMultipass(device, NULL, *(void **)(chunk_of(e) + 28), NULL);
            chunks++;
        }
        return;
    }

    int sel = lod_tables[variant][lod];
    void *format = NULL; // vertex format the state was last fully set up for
    for (uint8_t *e = *(uint8_t **)(bin + 12); e != *(uint8_t **)(bin + 16); e += 24) {
        uint8_t *chunk = chunk_of(e);
        int start = *(int32_t *)(chunk + sel * 8), count = *(int32_t *)(chunk + sel * 8 + 4);
        if (count <= 0)
            continue;
        void *mesh = *(void **)(chunk + 28);
        chunks++;
        if (can_draw_short(device, mesh, format) && (run.format == format || run_start(device, format))) {
            draw_short(device, mesh, start, count);
        } else {
            run_end(device);
            draw_full(device, mesh, start, count);
            format = MESH_FORMAT(mesh);
        }
    }
    run_end(device);
}

// TerrainRenderObj::DispatchDeferredDispatchBin(DeviceC*, DispatchBin&, ShaderLOD, unsigned, RenderPass):
// the remaining arguments are unused
static void DispatchBin_0(void *self, void *device, uint8_t *bin, int lod) { dispatch_bin(0, device, bin, lod); }
static void DispatchBin_1(void *self, void *device, uint8_t *bin, int lod) { dispatch_bin(1, device, bin, lod); }
static void *const hooks[NVARIANTS] = { DispatchBin_0, DispatchBin_1 };

void ssx_terrainmerge_patch(void) {
    uintptr_t base = so_mod.text_base;
    SetPrimRange = (void *)(base + OFF_SetPrimRange);
    MeshRenderImmediateMultipass = (void *)(base + OFF_MeshRenderImmediateMultipass);
    GetOGLES20 = (void *)(base + OFF_GetOGLES20);
    GetNumPasses = (void *)(base + OFF_GetNumPasses);
    UnbindMesh = (void *)(base + OFF_UnbindMesh);
    GetLocation = (void *)(base + OFF_GetLocation);
    sizes_table = (const uint8_t *)(base + OFF_SIZES_INSN + 8 + *(const int32_t *)(base + OFF_SIZES_LIT));
    types_table = (const uint8_t *)(base + OFF_TYPES_INSN + 8 + *(const int32_t *)(base + OFF_TYPES_LIT));
    norms_table = (const uint8_t *)(base + OFF_NORMS_INSN + 8 + *(const int32_t *)(base + OFF_NORMS_LIT));
    for (unsigned v = 0; v < NVARIANTS; v++) {
        // The function starts "push {r4-r10, lr}; ldr r5, [pc, #252]"
        const uint32_t *code = (const uint32_t *)(base + variants[v].func);
        if (code[0] != 0xE92D47F0 || code[1] != 0xE59F50FC) {
            l_error("[TERRAIN] unexpected DispatchDeferredDispatchBin at 0x%x (0x%08X 0x%08X), left as is",
                    variants[v].func, code[0], code[1]);
            continue;
        }
        lod_tables[v] = (const int32_t *)(base + variants[v].table_insn + 8 +
                                          *(const int32_t *)(base + variants[v].table_literal));
        hook_arm(base + variants[v].func, (uintptr_t)hooks[v]);
        kuKernelFlushCaches((void *)(base + variants[v].func), 8);
    }
    l_info("[TERRAIN] terrain chunks after the first of a bin take the short draw path (buffers and pointers only)");
}

void ssx_terrainmerge_report(uint32_t frames) {
    if (frames && chunks)
        l_info("[PERF] terrain: %u chunks per frame, %u of them by the short path; us per chunk: full path %.1f, "
               "short path bind %.1f + draw %.1f, unbind per run %.1f",
               chunks / frames, short_draws / frames, full_draws ? (float)t_full / full_draws : 0.0f,
               short_draws ? (float)t_bind / short_draws : 0.0f, short_draws ? (float)t_draw / short_draws : 0.0f,
               short_draws ? (float)t_unbind / short_draws : 0.0f);
    if (frames && vgl_ssx_draws)
        l_info("[PERF] vitaGL glDrawElements: %u per frame, us each: %.1f shader setup + %.1f sceGxm draw",
               vgl_ssx_draws / frames, (float)vgl_ssx_draw_setup_us / vgl_ssx_draws,
               (float)vgl_ssx_draw_gxm_us / vgl_ssx_draws);
    chunks = short_draws = full_draws = 0;
    t_full = t_bind = t_draw = t_unbind = 0;
    vgl_ssx_draws = vgl_ssx_draw_setup_us = vgl_ssx_draw_gxm_us = 0;
}
